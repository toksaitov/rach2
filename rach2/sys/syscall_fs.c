/*
 * The file system arm of the system call layer: descriptors, paths and the
 * credential model, open, read and write, the stat family, links, mounts
 * and directories. Its handlers are entered in the table by
 * `arch_syscall_init` (arch/i686/systab.c, arch/x86_64/systab.c); what it
 * shares with the other arms is in syscall_internal.h.
 */
#include "network/wireless/wiphy.h"
#include "arch/uabi.h"
#include "../kernel.h"
#include "mm/kheap.h"
#include "klog.h"
#include "syscall.h"
#include "power.h"
#include "serial.h"
#include "panic.h"
#include "display/blank.h"
#include "kio.h"
#include "kbd.h"
#include "input.h"
#include "mouse.h"
#include "proc/process.h"
#include "proc/sched.h"
#include "proc/tgroup.h"
#include "proc/ns.h"
#include "proc/elf.h"
#include "proc/uimage.h"
#include "proc/wait_queue.h"
#include "lib/krandom.h"
#include "lib/kerrno.h"
#include "lib/ksignal.h"
#include "lib/kirq.h"
#include "arch/arch.h"
#include "fs/fd.h"
#include "fs/debugfs.h"
#include "fs/file.h"
#include "proc/smp.h"
#include "dmabuf/dmabuf.h"
#include "fs/lock.h"
#include "fs/pathbuf.h"
#include "ipc/pipe.h"
#include "ipc/mqueue.h"
#include "io/poll.h"
#include "io/epoll.h"
#include "tty/tty.h"
#include "tty/pty.h"
#include "rtc.h"
#include "block/loop.h"
#include "fs/krffs.h"
#include "fs/krffsmount.h"
#include "fs/fat32.h"
#include "fs/exfat.h"
#include "fs/ext2.h"
#include "fs/devfs.h"
#include "fs/devtree.h"
#include "fs/synthfs.h"
#include "fs/sysfs.h"
#include "fs/vfs.h"
#include "fs/name.h"
#include "block/usbdisk.h"
#include "fs/procfs.h"
#include "fs/tmpfs.h"
#include "mm/paging.h"
#include "mm/pressure.h"
#include "mm/pagecache.h"
#include "mm/heap.h"
#include "mm/usercopy.h"
#include "mm/vm.h"
#include "lib/kuser.h"
#include "lib/kstring.h"
#include "display/backlight.h"
#include "rfkill.h"
#include "display/fb.h"
#include "speaker.h"
#include "audio/audio.h"
#include "network/socket/socket.h"
#include "network/socket/netlink.h"
#include "network/socket/packet.h"
#include "network/socket/raw.h"
#include "network/ethernet/ethernet.h"
#include "network/netif.h"
#include "network/driver/netdev.h"
#include "network/udp/udp.h"
#include "network/tcp/tcp.h"
#include "network/ip/ip.h"
#include "network/ip/icmp.h"
#include "network/net.h"
#include "io/inotify.h"
#include "ipc/unix.h"
#include "mm/filecache.h"
#include "mm/filemap.h"
#include "mm/mm.h"
#include "proc/coredump.h"
#include "timer.h"
#include "syscall_internal.h"

/*
 * The record from the heap that the `priv` of an O_PATH descriptor on a
 * synthetic leaf, FD_TYPE_SYNTH_PATH, points at: the node's classified path,
 * first so that `priv` reads as a `struct vpath`, and the owner its last
 * lookup gave it when its inode keeps one (struct synth_owner). The release
 * of the description's last reference frees it (`fd_release_objects`).
 */
struct synth_path {
    struct vpath vp;
    struct synth_owner own;
};

/**
 * A record from the heap of the synthetic directory an FD_TYPE_SYNTH_DIR
 * descriptor is open on, or NULL when the heap has no room. The kind, a
 * 32-bit pid and the tid do not fit in the descriptor's `priv` pointer, so
 * `priv` points at the record (struct synth_dir, synthfs.h), which the
 * release of the description's last reference frees (`fd_release_objects`).
 */
static struct synth_dir *synth_dir_alloc(int kind, int pid, int tid)
{
    struct synth_dir *sd = kzalloc(sizeof(*sd));

    if (sd) {
        sd->kind = kind;
        sd->pid = pid;
        sd->tid = tid;
        sd->used = 1;
    }
    return sd;
}

/**
 * Fill `vp` with the classified path of the synthetic directory whose record
 * is `sd`, or with VNODE_ENOENT when `sd` is NULL. A directory of /proc/sys
 * has its entry in the record's `pid`, and a directory of /sys its node and
 * device in `pid` and `tid`; `vp` has them in `sysent` and `syskey`.
 */
static void synth_dir_vpath(const struct synth_dir *sd, struct vpath *vp)
{
    k_memset(vp, 0, sizeof(*vp));
    vp->kind = sd ? sd->kind : VNODE_ENOENT;
    vp->pid = sd ? sd->pid : 0;
    vp->tid = sd ? sd->tid : 0;
    if (vp->kind == VNODE_PROC_SYS_DIR) {
        vp->sysent = vp->pid;
        vp->pid = 0;
    }
    if (vp->kind == VNODE_SYS_DIR) {
        vp->sysent = vp->pid;
        vp->syskey = vp->tid;
        vp->pid = 0;
        vp->tid = 0;
    }
}

/**
 * How many descriptors other than `except` hold `node` as their backing file.
 *
 * A scan of every process's descriptor table, and deliberately not a counter on
 * the node. A counter would have to be incremented in `fork`'s table copy, in
 * `dup`, in `dup2`, and in every future path that copies an `fd_entry_t`, and
 * one missed increment is a use-after-free that no test would show. The tables
 * are the count, so there is nothing to keep in step.
 *
 * The tables are a few pointer compares per live process, of kernel memory;
 * the scan never touches the page-cache window, and the node's own mode is
 * the one read of it. A descriptor in flight on an AF_UNIX socket, a file
 * mapping and an address space that runs the file as its program hold the
 * node as a descriptor does; any number of them counts as one holder more,
 * since every caller asks only whether there is a holder. The ends of a FIFO
 * hold it too, by their descriptions, whose descriptors name a pipe and no
 * node: every description of the FIFO counts, `except`'s only when a
 * descriptor besides it keeps it, since the FIFO's pipe is found by the
 * node's address, which a new node must not take while an end is open.
 */
static int fd_node_holders(const krffs_node_t *node, const fd_entry_t *except)
{
    int held = 0;
    process_t *pr;
    int i;

    if (!node) {
        return 0;
    }
    for (pr = proc_list; pr; pr = pr->next) {
        uint32_t flags;

        if (!pr->fds) {
            continue;
        }
        flags = fd_table_lock(pr->fds);
        for (i = 0; i < pr->fds->cap; i++) {
            fd_entry_t *e = fd_slot(pr->fds, i);

            if (e != except && fd_root_node(e) == node) {
                held++;
            }
        }
        fd_table_unlock(pr->fds, flags);
    }
    if (unix_inflight_holds(node) || filemap_holds(node) || mm_exe_holds(vfs_root_mount(), node)) {
        held++;
    }
    if (krffs_mode_type(node->mode) == KRFFS_S_IFIFO) {
        held += fd_ofd_holders(vfs_root_mount(), node, except ? except->ofd : NULL);
    }
    return held;
}

/**
 * Free `node`, an orphan of the KRFFS root whose last holder is `e`, after
 * dropping the record locks of the description of `e` when `e` has its last
 * reference. A lock of the root is keyed by its node's place
 * (`fd_lock_key`), and a node the allocator makes there later must meet none
 * of them; the description's own end drops them only after this, when
 * `fd_free` lets its last reference go. The caller holds the root's
 * namespace lock (`fd_release_lock`).
 */
static void fd_free_orphan(const fd_entry_t *e, krffs_node_t *node)
{
    if (e->ofd && e->ofd->refcount == 1 && lock_any()) {
        lock_ofd_gone(e->ofd);
    }
    krffs_remove_reserved_node(krffs_fs, node);
}

/**
 * Free the node of a FIFO of the KRFFS root that lost its name while open,
 * when `e`, an end of it or the reservation of an open of it that did not
 * finish, is the last to hold it: the release of a file's last descriptor
 * frees its orphan, and a FIFO's ends are its descriptors, opened through the
 * root or a bind of it. The caller holds the root's namespace lock.
 */
static void fd_fifo_orphan(const fd_entry_t *e)
{
    krffs_node_t *fifo;

    if (!e->ofd || !e->ofd->vmnt || !krffs_fs || !mnt_is_root(vfs_mount_base(e->ofd->vmnt))) {
        return;
    }
    fifo = e->ofd->vnode;
    if (fifo && fifo != krffs_fs->node && fifo->type == KRFFS_RESERVED_NODE &&
        krffs_mode_type(fifo->mode) == KRFFS_S_IFIFO && fifo->name[0] == '\0' &&
        fd_node_holders(fifo, e) == 0) {
        fd_free_orphan(e, fifo);
    }
}

/**
 * The unlink and the rename of a file hold that lock while they count its
 * holders and take its name, and every allocation on the root holds it, so
 * under it the release's count of the holders and its reclaim of the node,
 * which marks the node's extent free and may sleep reading a neighbouring
 * header, come with no count and no allocation between them. A holder that the
 * count finds in a list of its own, a file mapping or a descriptor in flight,
 * takes the lock before it leaves that list, as the address space that runs the
 * file does (`fs_exe_lock`): until then a release of the file's other holders
 * counts it. The release takes the lock again, which its holder may do.
 */
struct vfs_mount *fd_release_lock(const fd_entry_t *e)
{
    struct vfs_mount *root = vfs_root_mount();
    int orphanable;

    if (!krffs_fs) {
        return NULL;
    }
    if (e->type == FD_TYPE_FILE) {
        orphanable = fd_root_node(e) != NULL;
    } else {
        orphanable = (e->type == FD_TYPE_PIPE || e->type == FD_TYPE_NONE) && e->ofd &&
                     e->ofd->vmnt && vfs_mount_base(e->ofd->vmnt) == root && mnt_is_root(root);
    }
    if (!orphanable) {
        return NULL;
    }
    vfs_ns_lock(root);
    return root;
}

/**
 * Free the node of the KRFFS root that `e` holds, when the node lost its name
 * and `e` is its last holder: a file, or a FIFO whose end or reservation of
 * an open `e` is (`fd_fifo_orphan`). The count of the holders and the reclaim
 * run under the root's namespace lock (`fd_release_lock`). A descriptor in a
 * table stays there, and so among the holders, until the release that calls
 * this ends; a holder kept in a list of its own leaves the list under the
 * same lock, which it took first.
 */
static void fd_release_orphan(fd_entry_t *e)
{
    struct vfs_mount *locked = fd_release_lock(e);
    krffs_node_t *node;

    if (!locked) {
        return;
    }
    if (e->type == FD_TYPE_FILE) {
        node = fd_root_node(e);
        if (node->name[0] == '\0' && fd_node_holders(node, e) == 0) {
            fd_free_orphan(e, node);
            e->node = NULL;
            if (e->ofd) {
                e->ofd->vnode = NULL;
            }
        }
    } else {
        fd_fifo_orphan(e);
    }
    vfs_ns_unlock(locked);
}

int fs_node_is_held(const krffs_node_t *node)
{
    return fd_node_holders(node, NULL) > 0;
}

void fs_node_moved(const krffs_node_t *from, krffs_node_t *to)
{
    if (!from || from == to) {
        return;
    }
    vfs_repoint(vfs_root_mount(), from, to);
}

/**
 * Whether a lock on a file of `mnt` is found by the file's node, on a type
 * whose inode number does not stay with the file: KRFFS, the boot root or a
 * mounted volume, whose node is the file's place, which a growing write can
 * move and `vfs_repoint` moves every holder of; and FAT32 and exFAT, whose
 * node is one record per file while it is held, which a rename updates in
 * place, where the inode number is the file's directory entry.
 */
static int mnt_locks_by_node(const struct vfs_mount *mnt)
{
    return mnt->type == &vfs_krffs_type || mnt->type == &vfs_krffs_volume_type ||
           mnt->type == &vfs_vfat_type || mnt->type == &vfs_exfat_type;
}

/**
 * The key the locks of what `e` is open on are found by (lock.h), for a file
 * of a mounted type, a FIFO among them, its mount and a number that names
 * the file; and for any other descriptor the object it is open on, a pipe, a
 * socket, a device or a terminal, with no mount, so that two opens of one
 * object share their locks as two opens of a file do.
 *
 * A file of KRFFS, FAT32 or exFAT is named by its node as the description
 * has it now, which is the file itself while a description has it; a write
 * that moves a KRFFS node moves the locks with it (`lock_repoint`). Its
 * inode number would not do: the KRFFS root numbers a file by its directory
 * and the hash of its name, which a rename changes and two names can share,
 * a KRFFS volume by the node's offset, which a move changes, and FAT32 and
 * exFAT by the file's directory entry, which a rename changes. A file of
 * another type is named by the inode number its description recorded at
 * the open, which stays with the file.
 */
static void fd_lock_key(const fd_entry_t *e, struct lock_key *key)
{
    key->mnt = NULL;
    key->shown = 0;
    if (e->ofd && e->ofd->vmnt) {
        key->mnt = vfs_mount_base(e->ofd->vmnt);
        key->ino =
            mnt_locks_by_node(e->ofd->vmnt) ? (uint64_t)(uintptr_t)e->ofd->vnode : e->ofd->ino;
        key->shown = e->ofd->ino;
    } else if (e->dev) {
        key->ino = (uintptr_t)e->dev;
    } else if (e->priv) {
        key->ino = (uintptr_t)e->priv;
    } else if (e->sock) {
        key->ino = (uintptr_t)e->sock;
    } else {
        key->ino = (uint64_t)e->type;
    }
}

/** Whether the key of what `e` is open on is its node (`fd_lock_key`),
 * which a KRFFS write moves under the big lock (`vfs_repoint`), so that the
 * key is made under it. */
static int fd_locks_by_node(const fd_entry_t *e)
{
    return e->ofd && e->ofd->vmnt && mnt_locks_by_node(e->ofd->vmnt);
}

/**
 * Drop the record locks of the calling thread's descriptor table on what `e`
 * is open on, when `e` is one of the table's descriptors, as POSIX has every
 * close do; the table owns them for every thread and process that shares it
 * (`lock.h`), so a close by any of them drops them all. The copy of an entry
 * a file mapping or a message in flight keeps is not a descriptor of the
 * table (`fd_in_table`), and its release drops none; nor does the close of
 * an O_PATH descriptor, which Linux does not count as a close of the file.
 * Nothing is done when no lock exists, as Linux's locks_remove_posix looks
 * at the file's list first (fs/locks.c:2617-2631); the list has its own
 * lock, and the big lock is taken only for a file keyed by its node
 * (`fd_locks_by_node`), which a KRFFS write moves under it. A caller that
 * runs beside a lock command of another sharer marks the descriptor
 * closing under the table's lock first, which the command reads after it
 * sets its lock (`fd_lock_still_open`).
 */
static void fd_close_locks(const fd_entry_t *e)
{
    struct lock_key key;
    int big;

    if (!lock_any() || !e->ofd || (e->flags & FD_FLAG_PATH) || !current_proc ||
        !fd_in_table(current_proc->fds, e)) {
        return;
    }
    big = fd_locks_by_node(e);
    if (big) {
        bkl_enter();
    }
    fd_lock_key(e, &key);
    lock_close(&key);
    if (big) {
        bkl_exit();
    }
}

/**
 * Release what the descriptor `e` holds besides its slot and the table's
 * record locks: a node of the KRFFS root that lost its name and has `e` as
 * its last holder, the record of a synthetic directory, the classified path
 * an O_PATH descriptor of a /dev or /proc leaf keeps, the descriptor's
 * socket reference, and an object with operations (a pipe, a terminal, a
 * pty, the framebuffer), and then the descriptor's reference to its
 * description. Every release of a descriptor comes here, most by
 * `fd_release_backing`, so none can miss a kind of backing; the caller then
 * clears the slot with `fd_free_after`, or drops the copy it released, and
 * no longer reads `e->ofd`. May sleep.
 *
 * The record, the path and an object with operations belong to the open
 * file description, which dup, dup2 and fork share by its reference count,
 * so each is released once, when the description's last descriptor closes.
 * Which descriptor is the last is decided in one atomic step on the count
 * (`fd_ofd_unref_unless_last`), since two descriptors of one description
 * may close at once in two tables, one of them without the big lock; the
 * last takes the description's epoll registrations off the queues of what
 * it names (`epoll_release_file`), before the socket's reference goes,
 * releases the objects and then ends the description. The
 * framebuffer's close drops one open description's ownership reference
 * (display/fbdev.c). The socket reference is counted for each descriptor
 * (`socket_ref` on dup and fork) and is dropped every time. No descriptor
 * is both an object with operations and a socket.
 *
 * An orphan, a reserved node no path names, is freed by its last holder,
 * under the root's namespace lock (`fd_release_orphan`). This runs while
 * the closing slot is still populated, which is what the count's `except` is
 * for: the closing descriptor is skipped where it is, so every other release
 * counts it until its own has the lock.
 */
static void fd_release_objects(fd_entry_t *e)
{
    int last;

    fd_release_orphan(e);
    last = e->ofd && !fd_ofd_unref_unless_last(e->ofd);
    if (last) {
        epoll_release_file(e->ofd);
    }
    if (e->type == FD_TYPE_SOCKET && e->sock) {
        socket_unref(e->sock);
    }
    if (!last) {
        return;
    }
    if ((e->type == FD_TYPE_SYNTH_DIR || e->type == FD_TYPE_SYNTH_PATH) && e->priv) {
        procfs_owner_drop(e->type == FD_TYPE_SYNTH_DIR ? &((struct synth_dir *)e->priv)->own
                                                       : &((struct synth_path *)e->priv)->own);
        kfree(e->priv);
        e->priv = NULL;
    }
    if (e->ops && e->ops->close) {
        e->ops->close(e);
    }
    fd_ofd_unref(e->ofd);
}

void fd_release_backing(fd_entry_t *e)
{
    if (!e) {
        return;
    }
    fd_close_locks(e);
    fd_release_objects(e);
}

/**
 * A description of the file at `ref` for an address space to keep its
 * program by (`mm_exe_open`): one that opens nothing, as `path_install` makes
 * for O_PATH, with a hold of its own on the node and a reference on the
 * mount, so a volume is not unmounted while a program on it runs, as on
 * Linux; and on a type that keeps a node only while it is open, mounted
 * KRFFS, a pin, so a rename or an unlink leaves the file to it. On the KRFFS
 * root the address space counts as a holder of the node (`fd_node_holders`),
 * and a relocation moves the description's node (`fd_ofd_repoint`). Its
 * inode number is the file's, as an open gives it, which /proc/PID/maps
 * prints for the program's segments. NULL when the heap has no room or the
 * pin fails.
 */
struct open_file *fs_exe_open(const struct vfs_ref *ref)
{
    struct open_file *ofd = fd_ofd_new();
    struct vfs_stat st;

    if (!ofd) {
        return NULL;
    }
    if (!mnt_is_root(ref->mnt) && ref->mnt->type->ops->pin &&
        ref->mnt->type->ops->pin(ref->mnt, ref->node, &ofd->pin) != 0) {
        fd_ofd_unref(ofd);
        return NULL;
    }
    vfs_ref_hold(ref);
    ofd->vmnt = ref->mnt;
    ofd->vnode = ref->node;
    if (!mnt_is_root(ref->mnt)) {
        ofd->ino = vfs_getattr(ref->mnt, ref->node, &st) == 0 ? st.ino : 0;
    } else {
        ofd->ino = ref->node == krffs_fs->node ? 1 : krffs_node_ino(ref->node);
    }
    vfs_desc_open(ref->mnt);
    return ofd;
}

/**
 * Take the lock the release of `ofd`, the description of an address space's
 * program's file, needs, before the address space leaves the list
 * `mm_exe_holds` reads: for a file of the KRFFS root the root's namespace
 * lock, which the unlink and the rename of a file hold while they count its
 * holders and take its name, and which the allocation of an extent holds.
 * Under it no count of the file's holders runs between the address space's
 * leaving and the release, and no allocation takes the extent of a file the
 * release frees while its reclaim sleeps in the page cache. The mount
 * locked, or NULL for a file of another type, whose release needs no lock.
 * May sleep, and so is taken with no namespace lock held.
 */
struct vfs_mount *fs_exe_lock(const struct open_file *ofd)
{
    if (vfs_mount_base(ofd->vmnt) != vfs_root_mount() || ofd->vmnt->type != &vfs_krffs_type) {
        return NULL;
    }
    vfs_ns_lock(vfs_root_mount());
    return vfs_root_mount();
}

/**
 * Let go of `ofd`, the description an address space kept its program's file
 * by, once that address space is off the list `mm_exe_holds` reads, with
 * `locked` held as `fs_exe_lock` returned it: a file of the KRFFS root that
 * lost its name is freed when nothing else holds it, as the release of a
 * file's last descriptor frees one (`fd_release_orphan`, which takes the
 * lock again), at the node the file is at now; then the lock goes, and the
 * description's last reference closes it. May sleep.
 */
void fs_exe_close(struct open_file *ofd, struct vfs_mount *locked)
{
    fd_entry_t e;

    if (locked) {
        k_memset(&e, 0, sizeof(e));
        e.type = FD_TYPE_FILE;
        e.flags = FD_FLAG_PATH;
        e.ofd = ofd;
        if (ofd->vnode != krffs_fs->node) {
            e.node = ofd->vnode;
        }
        fd_release_orphan(&e);
        vfs_ns_unlock(locked);
    }
    fd_ofd_unref(ofd);
}

/** Whether pread64 can read `entry` at an offset, or it is a stream that
 * pread64 refuses with -ESPIPE: a pipe, a socket, a terminal or
 * pseudoterminal, the mouse, a device other than a raw window, the log or
 * /dev/null, /dev/zero and /dev/full, and an anonymous object with no
 * `llseek`. 1 or 0. Asked once, for FD_MODE_PREAD (`fd_mode`). */
static int pio_seekable_of(fd_entry_t *entry)
{
    if (entry->type == FD_TYPE_DEVICE) {
        return devfs_fd_is_raw(entry) || devfs_fd_log_size(entry, NULL) || devfs_lseek_zero(entry);
    }
    if (entry->type == FD_TYPE_ANON) {
        return entry->ops && entry->ops->llseek;
    }
    return entry->type == FD_TYPE_FILE || entry->type == FD_TYPE_DIR ||
           entry->type == FD_TYPE_SYNTH_DIR || entry->type == FD_TYPE_PROC ||
           entry->type == FD_TYPE_FRAMEBUFFER;
}

/**
 * Whether an ops-backed read or write of `entry` holds its description for
 * its length, as lseek holds it to move the offset. A raw disk window's
 * transfer uses `raw_offset`; a /proc file's the offset, a byte of its text
 * or an address of /proc/PID/mem; and a log device's the offset, a byte of
 * the kernel log's ring or of the screen log's text
 * (`devfs_fd_moves_offset`); and the framebuffer's the offset, a byte of
 * its pixels. Each may sleep before it sets the offset past
 * what it moved, in a disk request, or in a fault on the caller's buffer or
 * in bringing in a page of a process; a debugfs file's, whose operations
 * are its registrant's, may too. A seek through a descriptor that
 * shares the description must wait for it or be lost, and a second transfer
 * must wait to start from where the first leaves the offset. Linux holds
 * such a file's position lock across read, write and lseek alike. pread64
 * and pwrite64 hold nothing, since they work on a copy (`pio_view`).
 * Asked once, for FD_MODE_POS_HELD (`fd_mode`).
 */
static int ops_offset_moves(const fd_entry_t *entry)
{
    return devfs_fd_moves_offset((fd_entry_t *)(uintptr_t)entry) || entry->type == FD_TYPE_PROC ||
           entry->type == FD_TYPE_FRAMEBUFFER || entry->type == FD_TYPE_FILE;
}

static int file_rw_unlocked(const fd_entry_t *e);

/**
 * The FD_MODE_* bits of `e` (fd.h), decided now from its type, object,
 * device row and mounts, which stay as the open set them while it is open,
 * as Linux's do_dentry_open sets f_mode and FMODE_STREAM, and
 * FMODE_UNSIGNED_OFFSET is set by the open of /dev/mem and /proc/PID/mem:
 * whether pread64 takes it (`pio_seekable_of`); whether a read or write
 * checks its end at an offset, which a stream and a directory do not; which
 * offset, a raw window's own or the description's; whether that offset is
 * unsigned; whether an ops-backed transfer holds the description
 * (`ops_offset_moves`); and whether a read, and a write, runs without the
 * big lock: an object `file_rw_unlocked` admits, a file or directory
 * `file_vfs_unlocked` admits for a read, and a file
 * `file_vfs_writes_unlocked` admits for a write.
 */
static uint32_t fd_mode_of(fd_entry_t *e)
{
    uint32_t mode = FD_MODE_SET;
    int unlocked = file_rw_unlocked(e);

    if (pio_seekable_of(e)) {
        mode |= FD_MODE_PREAD;
        if (e->ofd && e->type != FD_TYPE_DIR && e->type != FD_TYPE_SYNTH_DIR) {
            mode |= FD_MODE_POS;
        }
    }
    if (devfs_fd_is_raw(e)) {
        mode |= FD_MODE_RAW_POS;
    }
    if (procfs_is_mem(e) || devfs_fd_is_mem(e)) {
        mode |= FD_MODE_UNSIGNED;
    }
    if (ops_offset_moves(e)) {
        mode |= FD_MODE_POS_HELD;
    }
    if (unlocked || file_vfs_unlocked(e)) {
        mode |= FD_MODE_READ_FREE;
    }
    if (unlocked || file_vfs_writes_unlocked(e)) {
        mode |= FD_MODE_WRITE_FREE;
    }
    return mode;
}

/**
 * The FD_MODE_* bits of `e`, decided by the first call that asks
 * (`fd_mode_of`) and kept in the entry for the rest. Two sharers of a table
 * that ask at once each store the same word, so the store takes no lock; a
 * copy of the entry made before it decides again.
 */
static uint32_t fd_mode(const fd_entry_t *e)
{
    uint32_t mode = e->mode;

    if (!(mode & FD_MODE_SET)) {
        mode = fd_mode_of((fd_entry_t *)(uintptr_t)e);
        ((fd_entry_t *)(uintptr_t)e)->mode = mode;
    }
    return mode;
}

/** Whether pread64 can read `entry` at an offset, or it is a stream that
 * pread64 refuses with -ESPIPE (`pio_seekable_of`): FD_MODE_PREAD. 1 or 0. */
static int pio_seekable(fd_entry_t *entry)
{
    return (fd_mode(entry) & FD_MODE_PREAD) != 0;
}

/**
 * Whether a transfer of `count` bytes at `pos` of `entry` would end past
 * 2^63-1, which Linux's rw_verify_area refuses with EINVAL: 1 or 0. `count`
 * is the request before its cut at USER_RW_MAX. /proc/PID/mem and /dev/mem
 * take unsigned offsets, as Linux's FMODE_UNSIGNED_OFFSET makes them, and
 * are never refused; a NULL `entry` is a directory, whose offsets are
 * signed.
 */
static int rw_end_past(fd_entry_t *entry, uint64_t pos, uint64_t count)
{
    const uint64_t max = 0x7FFFFFFFFFFFFFFFull;

    if (entry && (fd_mode(entry) & FD_MODE_UNSIGNED)) {
        return 0;
    }
    return pos > max || count > max - pos;
}

/**
 * Check a read or write of `count` bytes, the request before its cut, at
 * the offset of the description `entry` is open on: -EINVAL when its end
 * would pass 2^63-1 (`rw_end_past`), as Linux's ksys_read and ksys_write
 * check every file that is not a stream through rw_verify_area, or 0. Only
 * a descriptor with FD_MODE_POS has an offset to check: a stream, what
 * pread64 refuses, has none, and a directory's is its listing's cursor,
 * whose read is -EISDIR. A raw window moves an offset of its own,
 * `raw_offset` (FD_MODE_RAW_POS). A NULL `entry` passes.
 * The callers ask before the transfer, for Linux's order of errors, and the
 * transfer asks again under the description's lock, since a sharer's seek
 * can move the offset while the buffer is faulted in (`file_read`).
 */
static int32_t rw_offset_end(fd_entry_t *entry, size_t count)
{
    const uint64_t max = 0x7FFFFFFFFFFFFFFFull;
    uint32_t mode;
    uint64_t pos;

    if (!entry) {
        return 0;
    }
    mode = fd_mode(entry);
    if (!(mode & FD_MODE_POS) || (mode & FD_MODE_UNSIGNED)) {
        return 0;
    }
    pos = (mode & FD_MODE_RAW_POS) ? entry->ofd->raw_offset : entry->ofd->offset;
    return pos > max || (uint64_t)count > max - pos ? -EINVAL : 0;
}

/** The description an ops-backed read or write of `entry` holds for its
 * length (FD_MODE_POS_HELD, `ops_offset_moves`), or NULL. */
static struct open_file *ops_offset_held(fd_entry_t *entry)
{
    return (fd_mode(entry) & FD_MODE_POS_HELD) ? entry->ofd : NULL;
}

/** Dispatch an ops-backed write, holding the description of one whose write
 * uses its offset (`ops_offset_held`); -EBADF for a descriptor not open for
 * writing, and -EINVAL for an object without a write, such as an epoll set,
 * as Linux's vfs_write tests FMODE_CAN_WRITE after FMODE_WRITE, or for an
 * end of `asked` bytes, the request before its cut, past 2^63-1 at the
 * offset the held description has under its lock (`rw_offset_end`), which a
 * sharer's seek may have moved since the caller's check. */
static int32_t write_ops(fd_entry_t *entry, const char __user *buf, uint32_t count, size_t asked)
{
    struct open_file *held = ops_offset_held(entry);
    int32_t rc;

    if (!(entry->flags & FD_FLAG_WRITE)) {
        return -EBADF;
    }
    if (held) {
        fd_ofd_lock(held);
        if (rw_offset_end(entry, asked) != 0) {
            fd_ofd_unlock(held);
            return -EINVAL;
        }
    }
    rc = entry->ops->write ? entry->ops->write(entry, buf, count) : -EINVAL;
    if (held) {
        fd_ofd_unlock(held);
    }
    return rc;
}

/** Take the big lock unless the caller holds it: 1 when taken, for
 * `big_give`, as a write of a file `file_vfs_writes_unlocked` admits takes
 * it for the steps that still need it. */
static int big_take(void)
{
    if (bkl_held()) {
        return 0;
    }
    bkl_enter();
    return 1;
}

/** Give back the big lock `big_take` took when `taken` says it did. */
static void big_give(int taken)
{
    if (taken) {
        bkl_exit();
    }
}

/**
 * Whether the caller's RLIMIT_FSIZE refuses a truncate(2) that sets the size
 * of the file of `ofd` to `end` bytes: `vfs_newsize_ok` of the file's size,
 * which refuses growth past the limit alone. The caller holds the lock that
 * covers a change of the file's size (`file_lock`, or `size_ns_lock` on a
 * type that takes neither it nor the description's namespace lock), so that
 * the size read here is the size changed. A size that cannot be read is
 * that error.
 */
static int32_t fsize_growth_refused(const struct open_file *ofd, uint64_t end)
{
    unsigned long limit = current_proc->rlim[RLIM_FSIZE].cur;
    struct vfs_stat st;
    int32_t rc;

    if (limit == RLIM_INFINITY || end <= limit) {
        return 0;
    }
    rc = vfs_getattr(ofd->vmnt, ofd->vnode, &st);
    if (rc != 0) {
        return rc;
    }
    return vfs_newsize_ok(st.size, end);
}

/** The part of `count` bytes written at `pos` that the caller's
 * RLIMIT_FSIZE lets the file take: `count`, less, or 0 from the limit on. */
static uint32_t fsize_fit(uint64_t pos, uint32_t count)
{
    unsigned long limit = current_proc->rlim[RLIM_FSIZE].cur;

    if (limit == RLIM_INFINITY) {
        return count;
    }
    if (pos >= limit) {
        return 0;
    }
    return (uint64_t)count > limit - pos ? (uint32_t)(limit - pos) : count;
}

/**
 * Take the lock of the file the description `ofd` is open on for a write or
 * a change of its size, on a type that sets `file_locked` (`vfs_node_lock`),
 * into `file`: 1 when taken, which `file_unlock` gives back, and 0 on any
 * other type, whose description lock and namespace lock cover the change.
 * The caller holds the description's lock, which comes first.
 */
static int file_lock(const struct open_file *ofd, struct vfs_ref *file)
{
    file->mnt = ofd->vmnt;
    file->node = ofd->vnode;
    if (!vfs_file_locked(ofd->vmnt)) {
        return 0;
    }
    vfs_node_lock(file);
    return 1;
}

/** Give back the lock `file_lock` took when `own` says it did. */
static void file_unlock(const struct vfs_ref *file, int own)
{
    if (own) {
        vfs_node_unlock(file);
    }
}

/**
 * Take the namespace lock of the mount of `ofd` for a change of the file's
 * size on a type that neither takes it with the description's lock
 * (`ns_serialised`) nor locks each file (`file_locked`): FAT32 and exFAT,
 * whose `truncate` takes it only for itself. Held from before the size is
 * read for the RLIMIT_FSIZE check to after the change, it keeps another
 * truncation from moving the size between the two, as Linux holds the
 * inode's lock across both (fs/open.c:63 in 6.12). 1 when taken, which
 * `size_ns_unlock` gives back, and 0 on any other type. The caller holds
 * the description's lock, which comes first.
 */
static int size_ns_lock(const struct open_file *ofd)
{
    if (ofd->vmnt->type->ns_serialised || vfs_file_locked(ofd->vmnt)) {
        return 0;
    }
    vfs_ns_lock(ofd->vmnt);
    return 1;
}

/** Give back the lock `size_ns_lock` took when `held` says it did. */
static void size_ns_unlock(const struct open_file *ofd, int held)
{
    if (held) {
        vfs_ns_unlock(ofd->vmnt);
    }
}

/* The most times a write brings its buffer in again with no byte moved
 * between, before it answers -EFAULT. */
#define WRITE_REFAULTS 8

/* The file lock and per-operation append choice of one write or vector. */
struct file_write_state {
    struct vfs_ref file;
    int own;    /* nonzero when the type's file lock is held */
    int append; /* end-of-file placement without changing O_APPEND */
    /* nonzero once this call, an earlier vector element included, has moved
     * a byte */
    int moved;
    /* nonzero once this call has taken the big lock for a file mappings
     * hold pages of (`file_write_mapped`), which its caller gives back */
    int big;
};

/* The most bytes one piece of a write to a file whose pages mappings hold
 * copies from the caller (`file_write_piece`). */
#define WRITE_SNAPSHOT_BYTES 4096u

/* The kernel copy a write to a file whose pages mappings hold is made from
 * (`file_write_piece`): a heap buffer of WRITE_SNAPSHOT_BYTES, made at the
 * first such piece, or `small` when the heap has no room; and what the
 * last piece did. */
struct write_snap {
    uint8_t *buf;  /* NULL until the first piece that needs a copy */
    uint32_t size; /* the bytes `buf` holds */
    /* The last piece was copied and the file system wrote all it was given,
     * so the next piece follows at once. */
    int next;
    /* The last piece was to be copied and the caller's first page of it was
     * not present, so nothing was written. */
    int absent;
    uint8_t small[256];
};

/**
 * Whether the pages mappings hold of the file of `ofd` must take the bytes
 * a write moves (`filemap_disk_mapped`), asked under the file's lock before
 * each piece, which keeps a no true until it is let go, since a page joins
 * a file's pages only under that lock. Their copy (`filemap_disk_written`)
 * walks them under the big lock, which is taken for the rest of the call
 * when the caller lacks it and recorded in `state`.
 */
static int file_write_mapped(const struct open_file *ofd, struct file_write_state *state)
{
    if (!filemap_disk_mapped(ofd->vmnt, ofd->ino)) {
        return 0;
    }
    if (!bkl_held()) {
        bkl_enter();
        state->big = 1;
    }
    return 1;
}

/** Whether all `len` bytes at `addr` of the caller's memory are present
 * (`vm_resident`), asked under the big lock, which is taken when the
 * caller lacks it. */
static int user_resident(uintptr_t addr, uint32_t len)
{
    int big = big_take();
    int resident = vm_resident(addr, len, 0);

    big_give(big);
    return resident;
}

/**
 * One piece of a write, `len` bytes of the caller's memory at `from` to
 * `pos`. When mappings hold pages of the file (`file_write_mapped`), asked
 * under the file's lock, which then holds the big lock as well, at most
 * the size of `snap`'s buffer is copied into it, as ext2's commit reads the
 * caller, while the bytes are present (`vm_resident`), stopping at the
 * first page of the caller's that is not; the file is written from that
 * copy and the same bytes put into the file's pages (`filemap_disk_written`),
 * so that every byte the file system accepts is in the pages before the
 * lock goes, with no read and no failure path, and no write-back of a page
 * can put older bytes over the write. Otherwise the file system copies from
 * the caller itself. `snap` says what the piece did. Returns the bytes
 * written or the write's error.
 */
static int32_t file_write_piece(const struct open_file *ofd, struct file_write_state *state,
                                uintptr_t from, uint32_t len, uint64_t pos, struct write_snap *snap)
{
    uint32_t take, page_left = PAGE_SIZE - (uint32_t)(from % PAGE_SIZE);
    int present;
    int32_t n;

    snap->next = snap->absent = 0;
    if (!file_write_mapped(ofd, state)) {
        return vfs_write(ofd->vmnt, ofd->fhandle, (const char *)from, len, pos);
    }
    if (!snap->buf) {
        snap->buf = kmalloc(WRITE_SNAPSHOT_BYTES);
        snap->size = snap->buf ? WRITE_SNAPSHOT_BYTES : (uint32_t)sizeof(snap->small);
        snap->buf = snap->buf ? snap->buf : snap->small;
    }
    take = len < snap->size ? len : snap->size;
    present = vm_resident(from, take, 0);
    if (!present && take > page_left) {
        take = page_left;
        present = vm_resident(from, take, 0);
    }
    if (!present) {
        snap->absent = 1;
        return 0;
    }
    k_memcpy(snap->buf, (const void *)from, take);
    n = vfs_write(ofd->vmnt, ofd->fhandle, snap->buf, take, pos);
    if (n > 0) {
        filemap_disk_written(ofd->vmnt, ofd->ino, pos, snap->buf, (uint32_t)n);
    }
    snap->next = n > 0 && (uint32_t)n == take;
    return n;
}

/**
 * The writes of `file_write`: `count` bytes of `buf` from `*pos`, which
 * moves past each piece written. Before each piece the file's lock is asked
 * whether mappings hold pages of the file (`file_write_piece`), since a
 * fault while the lock was let go may have brought one in: when they do,
 * the piece is written from a kernel copy of the caller's bytes, which the
 * pages take too, in pieces of at most WRITE_SNAPSHOT_BYTES, a heap
 * buffer's or a smaller stack buffer's when the heap has no room. Returns
 * the bytes written, or the first write's error when none were.
 *
 * On a type that sets `file_locked`, `state` names the file whose lock
 * the caller holds, with `own` clear on another type. The file system
 * copies from `buf`
 * only while its pages are present (`vm_resident`), answering a short count
 * at the first that is not, since a fault under the lock could wait for
 * another file's lock. Once this call has moved a byte, in this piece or an
 * earlier vector element, it returns that prefix without releasing the lock.
 * Otherwise the lock goes while the buffer is brought in, and an append
 * reads the end again, where another writer may have moved it meanwhile:
 * the count is cut again to what RLIMIT_FSIZE lets through
 * (`fsize_fit`), and a write that finds the end at the limit stops there,
 * -EFBIG when it has moved no byte. An end that cannot be read again ends
 * the write with the bytes moved.
 */
static int32_t file_write_pieces(fd_entry_t *entry, struct file_write_state *state,
                                 const char __user *buf, uint32_t count, uint64_t *pos)
{
    struct open_file *ofd = entry->ofd;
    uintptr_t from = (__force uintptr_t)buf;
    struct write_snap snap;
    uint32_t done = 0;
    int refaults = 0;
    int32_t n;

    snap.buf = NULL;
    for (;;) {
        n = file_write_piece(ofd, state, from + done, count - done, *pos, &snap);
        if (n > 0) {
            done += (uint32_t)n;
            state->moved = 1;
            *pos += (uint64_t)n;
            refaults = 0;
        }
        if (n < 0 || done == count) {
            break;
        }
        if (snap.next) {
            continue;
        }
        if (!state->own || (!snap.absent && user_resident(from + done, count - done))) {
            break;
        }
        if (state->moved) {
            n = -EFAULT;
            break;
        }
        vfs_node_unlock(&state->file);
        n = ++refaults <= WRITE_REFAULTS && vm_prefault_range(from + done, count - done, 0)
                ? 0
                : -EFAULT;
        vfs_node_lock(&state->file);
        if (n != 0) {
            break;
        }
        if (state->append) {
            struct vfs_stat vst;

            n = vfs_getattr(ofd->vmnt, ofd->vnode, &vst);
            if (n != 0) {
                break;
            }
            *pos = vst.size;
            if (fsize_fit(*pos, count - done) == 0) {
                n = done > 0 ? 0 : vfs_fsize_refused(*pos + 1);
                break;
            }
            count = done + fsize_fit(*pos, count - done);
        }
    }
    if (snap.buf && snap.buf != snap.small) {
        kfree(snap.buf);
    }
    return done > 0 ? (int32_t)done : n;
}

/**
 * Write with the description and the locks in `state` already held. Its
 * `own` says the type uses a file lock; `append` is the operation's final
 * end-placement choice, independent of the shared O_APPEND flag. The caller
 * has faulted in the buffer; a later fault ends a call that moved bytes,
 * or releases and reacquires the file lock through file_write_pieces.
 * Return bytes or a negative errno,
 * the getattr's error among them when the end of an append cannot be read.
 *
 * On a type that does not set `file_locked` the description's lock orders
 * only the writes through that description, so an append there, and a
 * write to a file shared mappings hold, takes the mount's namespace lock
 * (`vfs_node_lock`) from before the size is read to after the bytes are
 * written: another open's append then waits and finds the new end. The
 * type's own write takes the same lock again, which its holder may.
 */
static int32_t file_write_locked(fd_entry_t *entry, const char __user *buf, uint32_t count,
                                 const uint64_t *at, struct file_write_state *state)
{
    struct open_file *ofd = entry->ofd;
    int append = state->append;
    int mapped = file_write_mapped(ofd, state);
    int held = !state->own && (append || mapped);
    uint64_t pos;
    int32_t written;

    if (held) {
        vfs_node_lock(&state->file);
    }
    pos = at ? *at : ofd->offset;
    if (append) {
        struct vfs_stat vst;
        int rc = vfs_getattr(ofd->vmnt, ofd->vnode, &vst);

        if (rc != 0) {
            written = rc;
            goto out;
        }
        pos = vst.size;
    }
    if (count > 0 && vfs_fsize_refused(pos + 1) != 0) {
        written = -EFBIG;
        goto out;
    }
    count = fsize_fit(pos, count);
    written = file_write_pieces(entry, state, buf, count, &pos);
out:
    if (held) {
        vfs_node_unlock(&state->file);
    }
    if (written > 0) {
        if (!cred_fs_privileged()) {
            if (entry->node) {
                cred_kill_setid(entry->node);
            } else {
                vfs_kill_setid(ofd->vmnt, ofd->vnode);
            }
        }
        if (!at) {
            ofd->offset = pos;
        }
    }
    return written;
}

static long sync_entry(fd_entry_t *e,
                       int (*sync_op)(struct vfs_mount *mnt, void *handle, uint32_t *seen));

/**
 * The end of a write of `written` bytes, or of its -errno, through `entry`:
 * on a description opened with O_SYNC the file synced as fsync syncs it,
 * and with O_DSYNC as fdatasync does, as Linux's `generic_write_sync`
 * (include/linux/fs.h), which syncs the range written where this syncs the
 * file. The caller holds no lock of the write's. Returns `written`, or the
 * sync's -errno.
 */
static long write_synced(fd_entry_t *entry, long written)
{
    uint32_t st = entry->ofd ? entry->ofd->status_flags : 0u;
    long rc;

    if (written <= 0 || !(st & OFD_DSYNC)) {
        return written;
    }
    rc = sync_entry(entry, (st & OFD_SYNC) == OFD_SYNC ? vfs_fsync : vfs_fdatasync);
    return rc < 0 ? rc : written;
}

/**
 * Write `count` bytes of user buffer `buf` to the file of a mounted type or
 * of the KRFFS root that `entry` is open on: at the description's offset,
 * which moves past what was written, or at `*at` for pwrite64, which leaves
 * it where it was. At the description's offset the end of `asked` bytes,
 * the request before its cut at USER_RW_MAX, is checked again under the
 * description's lock (`rw_offset_end`), since a sharer may have moved the
 * offset while the buffer was faulted in, as Linux checks it under the
 * position lock; `asked` is unused with `at`.
 *
 * Returns the bytes written, or a negative errno: -EBADF for a descriptor
 * not open for writing or not on a file of a mount, -EFAULT, -EBUSY, -EFBIG,
 * -EINVAL for that end past 2^63-1, or the file system's error. Takes the
 * description's lock, then the file's (`vfs_node_lock`): on a type that sets
 * `file_locked` always, and on another, where it is the mount's namespace
 * lock, for an O_APPEND write and for a file shared mappings hold; each from
 * before the size O_APPEND reads to after the write. May sleep.
 *
 * With O_APPEND every write lands at the current end of the file, whatever
 * the offset says and whatever `at` asks for, as Linux's pwrite does. The
 * file system reads the user buffer directly, so its pages are range-checked
 * and faulted in first, and on a type that sets `file_locked` brought in
 * again with the file's lock let go when one goes before any bytes move;
 * a later fault returns the completed prefix (`file_write_pieces`). A file
 * that backs a loop device is reached through that device and nowhere else,
 * since the file system mounted on it may not have its image change
 * underneath, and is -EBUSY: that is asked before the description is locked
 * and again under the lock, since an attachment can publish itself while
 * this call waits for the lock. Only a write that moved bytes disarms the
 * set-id bits, through the node as the write left it (a
 * grow relocates a KRFFS file and repoints every holder), and a privileged
 * writer is exempt: that is the CAP_FSETID analogue, without which any root
 * process disarms /bin/su by rewriting it. A file whose pages shared
 * mappings hold in memory is written from a kernel copy of the caller's
 * bytes, which the pages take under the file's lock, held from before the
 * write, so that no write-back of the pages comes between the two
 * (`file_write_piece`). Under
 * RLIMIT_FSIZE a write that would pass the limit writes what fits, and one
 * that starts at it fails (`vfs_fsize_refused`).
 *
 * A file `file_vfs_writes_unlocked` admits is written without the big lock,
 * under the description's lock and the file's, as Linux's
 * generic_file_write_iter writes under the inode's lock alone
 * (mm/filemap.c:4191-4206 in 6.12): the type takes the big lock where it
 * still needs it, and this call takes it for the copy into the pages
 * mappings hold (`file_write_mapped`), the look at the buffer's pages
 * (`user_resident`), the signal and the set-id change, and inotify's report.
 * On an O_SYNC or O_DSYNC description the write ends with the file's sync
 * (`write_synced`), with every lock of the write let go.
 */
static int32_t file_write(fd_entry_t *entry, const char __user *buf, uint32_t count,
                          const uint64_t *at, size_t asked)
{
    struct open_file *ofd = entry->ofd;
    struct file_write_state state;
    int32_t written;

    if (!(entry->flags & FD_FLAG_WRITE)) {
        return -EBADF;
    }
    if (!ofd || !ofd->vmnt) {
        return -EBADF;
    }
    if (!user_buffer_ok((__force uintptr_t)buf, count, 0)) {
        return -EFAULT;
    }
    if (loop_backs(ofd->vmnt, ofd->vnode)) {
        return -EBUSY;
    }
    fd_ofd_lock(ofd);
    if (!at && rw_offset_end(entry, asked) != 0) {
        fd_ofd_unlock(ofd);
        return -EINVAL;
    }
    state.own = file_lock(ofd, &state.file);
    state.append = (ofd->status_flags & OFD_APPEND) != 0;
    state.moved = 0;
    state.big = 0;
    if (loop_backs(ofd->vmnt, ofd->vnode)) {
        file_unlock(&state.file, state.own);
        fd_ofd_unlock(ofd);
        return -EBUSY;
    }
    written = file_write_locked(entry, buf, count, at, &state);
    file_unlock(&state.file, state.own);
    fd_ofd_unlock(ofd);
    if (written > 0 && inotify_watches) {
        if (!state.big) {
            state.big = big_take();
        }
        inotify_desc(ofd, IN_MODIFY);
    }
    big_give(state.big);
    return (int32_t)write_synced(entry, written);
}

/**
 * Write `count` bytes of `buf` to the console through the console
 * descriptor `entry`, for `write_fd`.
 *
 * The bytes go in pieces through a 256-byte kernel buffer and the
 * terminal's output processing (OPOST and ONLCR, so a bare newline becomes
 * CR LF on the screen and on the serial line). A fault on the user buffer
 * returns the bytes already written, or -EFAULT when there are none. While
 * output is stopped, by VSTOP under IXON or by TCOOFF, the write waits for it
 * to restart (`tty_wait_output`), or returns the bytes written, or -EAGAIN
 * when none, on a nonblocking description, as Linux's n_tty_write. A
 * process of a background group of the console's session writes freely
 * unless TOSTOP is set; with it set, SIGTTOU goes to the process group and
 * the call is -EINTR, unless the process blocks or ignores SIGTTOU, when it
 * writes, or its group is orphaned, when the call is -EIO
 * (`tty_job_check`), as Linux's n_tty_write.
 */
static int32_t console_write(fd_entry_t *entry, const char __user *buf, uint32_t count)
{
    char kbuf[256];
    uint32_t done = 0;
    int nonblock = entry->ofd && (entry->ofd->status_flags & OFD_NONBLOCK);
    uint32_t opened = entry->ofd ? entry->ofd->tty_hangups : console_tty.vhangups;

    if (!(entry->flags & FD_FLAG_WRITE)) {
        return -EBADF;
    }
    if (fd_tty_hung(entry)) {
        return -EIO;
    }
    if (console_tty.ldisc.tio.c_lflag & TOSTOP) {
        int rc = tty_job_check(&console_tty, SIGTTOU);

        if (rc != 0) {
            return rc;
        }
    }
    while (done < count) {
        uint32_t chunk = count - done;
        uint32_t put = 0;

        if (chunk > sizeof(kbuf)) {
            chunk = sizeof(kbuf);
        }
        if (copy_from_user(kbuf, (const void __user *)(buf + done), chunk) != 0) {
            return (done > 0) ? (int32_t)done : -EFAULT;
        }
        for (;;) {
            int32_t rc;

            put += (uint32_t)tty_write(&console_tty, (const unsigned char *)kbuf + put,
                                       (int)(chunk - put));
            if (put == chunk) {
                break;
            }
            rc = tty_wait_output(&console_tty, nonblock, opened);
            if (rc < 0) {
                return done + put > 0 ? (int32_t)(done + put) : rc;
            }
        }
        done += chunk;
        sched_service_point();
    }
    return (int32_t)count;
}

/** Check a scalar I/O request before its count is cut at USER_RW_MAX.
 * Return -EFAULT for a range outside user memory and the current kernel
 * caller's window, then -EINVAL for a count negative as the kernel's
 * ssize_t, or 0. This is Linux's access_ok and rw_verify_area order;
 * neither check touches the buffer. */
static int32_t rw_buffer_range(uintptr_t addr, size_t count)
{
    if (!user_span_ok(addr, count) && !proc_window_covers(current_proc, addr, count)) {
        return -EFAULT;
    }
    return (long)count < 0 ? -EINVAL : 0;
}

/** Check the descriptor's write access before its buffer or vector.
 * Return -EBADF for a missing descriptor, O_PATH, no write access or an
 * unsupported kind, -EINVAL for an object without a write method, or 0.
 * A socket's send checks its protocol after this, as Linux's does. */
static int32_t write_allowed(const fd_entry_t *entry)
{
    if (!entry || (entry->flags & FD_FLAG_PATH)) {
        return -EBADF;
    }
    if (entry->ops) {
        if (!(entry->flags & FD_FLAG_WRITE)) {
            return -EBADF;
        }
        return entry->ops->write ? 0 : -EINVAL;
    }
    if (entry->type == FD_TYPE_SOCKET) {
        return entry->sock ? 0 : -EBADF;
    }
    if (entry->type == FD_TYPE_CONSOLE) {
        return (entry->flags & FD_FLAG_WRITE) ? 0 : -EBADF;
    }
    if (entry->type == FD_TYPE_FILE) {
        return (entry->flags & FD_FLAG_WRITE) && entry->ofd && entry->ofd->vmnt ? 0 : -EBADF;
    }
    return -EBADF;
}

/**
 * Write up to `requested` bytes of user buffer `buf` to descriptor
 * `entry`, the caller's lookup, which may be NULL.
 *
 * The shared body of write(2) and of each element of writev(2). Returns the
 * bytes written or a negative errno: -EBADF for a descriptor that is not
 * open, not open for writing, or of a kind with no write, and -EINVAL for
 * one open for writing whose object has no write method (`write_ops`).
 * After those checks, rw_buffer_range checks the original range and count,
 * and rw_offset_end the end at the description's offset, -EINVAL past
 * 2^63-1, before USER_RW_MAX limits the transfer.
 *
 * It dispatches by backing kind. An object with operations (a pipe, a
 * terminal, a pty, a device) writes through them once `write_ops` has
 * checked that the descriptor was opened for writing, as Linux does. A stream
 * socket's send copies from the user buffer itself and sleeps for room in
 * its ring unless the description is non-blocking, and its EPIPE posts
 * SIGPIPE when `sigpipe` is set (`sock_stream_sent`). A datagram socket sends
 * to the peer it connected to, -EDESTADDRREQ without one, and a datagram
 * past 65535 bytes is -EMSGSIZE; a socket of another type is -EOPNOTSUPP.
 * A file of a mount or of the KRFFS root is `file_write`'s. The framebuffer
 * has operations of its own (display/fbdev.c), and the console is
 * `console_write`'s.
 */
static int32_t write_entry(fd_entry_t *entry, const char __user *buf, size_t requested, int sigpipe)
{
    uint32_t count;
    int32_t rc;

    if (!current_proc) {
        return -ESRCH;
    }

    rc = write_allowed(entry);
    if (rc != 0) {
        return rc;
    }
    rc = rw_buffer_range((__force uintptr_t)buf, requested);
    if (rc == 0) {
        rc = rw_offset_end(entry, requested);
    }
    if (rc != 0) {
        return rc;
    }
    count = USER_RW_LEN(requested);

    if (entry->ops) {
        return write_ops(entry, buf, count, requested);
    }

    if (entry->type == FD_TYPE_SOCKET) {
        if (!entry->sock) {
            return -EBADF;
        }
        if (entry->sock->type == SOCK_STREAM) {
            if (!user_buffer_ok((__force uintptr_t)buf, count, 0)) {
                return -EFAULT;
            }
            return sock_stream_sent(
                socket_tcp_send(entry->sock, (const uint8_t __force *)buf, (int)count,
                                entry->ofd && (entry->ofd->status_flags & OFD_NONBLOCK)),
                sigpipe);
        }
        if (entry->sock->type == SOCK_DGRAM) {
            if (!entry->sock->connected) {
                return -EDESTADDRREQ;
            }
            if (!user_buffer_ok((__force uintptr_t)buf, count, 0)) {
                return -EFAULT;
            }
            if (count > 0xFFFFu) {
                return -EMSGSIZE;
            }
            return dgram_send(entry->sock, entry->sock->remote_ip, entry->sock->remote_port,
                              (const uint8_t __force *)buf, (uint16_t)count);
        }
        return -EOPNOTSUPP;
    }

    if (entry->type == FD_TYPE_CONSOLE) {
        return console_write(entry, buf, count);
    }

    if (entry->type == FD_TYPE_FILE) {
        return file_write(entry, buf, count, NULL, requested);
    }

    return -EBADF;
}

/** `write_entry` for descriptor `fd`, which it looks up. */
static int32_t write_fd(int fd, const char __user *buf, size_t requested, int sigpipe)
{
    return write_entry(current_proc ? fd_get(current_proc->fds, fd) : NULL, buf, requested,
                       sigpipe);
}

/**
 * Whether descriptor `fd` takes a write, asked without writing, for a
 * writev(2) of no bytes, which Linux answers with 0 once the file is known
 * to be open for writing and to have a write (`vfs_writev`), never calling
 * the write itself: /dev/full and an unconnected datagram socket return 0
 * there. Returns 0, or -EBADF for a descriptor that is not open, is an
 * O_PATH one, is not open for writing, or is of a kind with no write, and
 * -EINVAL for one open for writing whose object has no write method, as an
 * epoll set has none, which Linux tests after the mode (FMODE_CAN_WRITE):
 * the refusals `write_fd` gives before it writes.
 */
static int32_t write_check(int fd)
{
    if (!current_proc) {
        return -ESRCH;
    }
    return write_allowed(fd_get(current_proc->fds, fd));
}

static int size_guarded(struct vfs_mount *mnt, void *node);
static int ofd_append_only(const struct open_file *ofd);
static int fd_change_lock(int fd);
static int rw_call_lock(const fd_entry_t *e, uint32_t free);

/**
 * A change clears set-user-ID, and set-group-ID where group execute is set, for
 * a caller without privilege, as a write does. Whether the file backs a loop
 * device is asked before the description is locked and again under the lock,
 * since an attachment can publish itself while this call waits for it. Shared
 * mappings of the file lose the pages past the new end
 * (`filemap_file_truncated`), under the file's lock on a type that sets
 * `file_locked` (`file_lock`), so that no write-back of a mapped page past
 * the new end comes between the two and no fault brings one in again; the
 * pages are cut under the big lock, taken when the caller lacks it, as is
 * inotify's report. A length that grows the file past the caller's
 * RLIMIT_FSIZE is -EFBIG with SIGXFSZ, judged under the same locks
 * (`fsize_growth_refused`), and on FAT32 and exFAT under the namespace lock
 * (`size_ns_lock`); a shrink is never refused. 0 or a negative errno.
 */
static long ftruncate_fd(int fd, long long length)
{
    fd_entry_t *entry;
    struct vfs_ref file;
    int own, ns, big = 0;
    int rc;

    if (!current_proc) {
        return -ESRCH;
    }
    if (length < 0) {
        return -EINVAL;
    }
    entry = fd_get(current_proc->fds, fd);
    if (!entry || (entry->flags & FD_FLAG_PATH)) {
        return -EBADF;
    }
    if (entry->type != FD_TYPE_FILE || !(entry->flags & FD_FLAG_WRITE)) {
        return -EINVAL;
    }
    if (!entry->ofd || !entry->ofd->vmnt) {
        return -EBADF;
    }
    if (size_guarded(entry->ofd->vmnt, entry->ofd->vnode)) {
        return -EPERM;
    }
    if (loop_backs(entry->ofd->vmnt, entry->ofd->vnode)) {
        return -EBUSY;
    }
    fd_ofd_lock(entry->ofd);
    own = file_lock(entry->ofd, &file);
    if (loop_backs(entry->ofd->vmnt, entry->ofd->vnode)) {
        file_unlock(&file, own);
        fd_ofd_unlock(entry->ofd);
        return -EBUSY;
    }
    ns = size_ns_lock(entry->ofd);
    rc = fsize_growth_refused(entry->ofd, (uint64_t)length);
    if (rc == 0) {
        rc = vfs_truncate(entry->ofd->vmnt, entry->ofd->fhandle, (uint64_t)length);
    }
    size_ns_unlock(entry->ofd, ns);
    if (rc == 0 && !cred_fs_privileged()) {
        if (entry->node) {
            cred_kill_setid(entry->node);
        } else {
            vfs_kill_setid(entry->ofd->vmnt, entry->ofd->vnode);
        }
    }
    if (rc == 0 && own && filemap_disk_mapped(entry->ofd->vmnt, entry->ofd->ino)) {
        big = big_take();
        filemap_file_truncated(entry->ofd->vmnt, entry->ofd->ino, (uint64_t)length);
    }
    file_unlock(&file, own);
    fd_ofd_unlock(entry->ofd);
    if (rc == 0) {
        if (!own) {
            filemap_file_truncated(entry->ofd->vmnt, entry->ofd->ino, (uint64_t)length);
        }
        if (inotify_watches) {
            if (!big) {
                big = big_take();
            }
            inotify_desc(entry->ofd, IN_MODIFY);
        }
    }
    big_give(big);
    return rc;
}

/**
 * Truncate the file of descriptor `fd` to `length` bytes, for ftruncate(2)
 * and ftruncate64(2): `ftruncate_fd`, under the big lock unless the
 * descriptor names a file `file_vfs_writes_unlocked` admits, which is
 * truncated under its description's lock and its own, as Linux's
 * do_ftruncate truncates under the inode's lock (fs/open.c:63-65 in 6.12),
 * the type taking the big lock where it still needs it. Returns as
 * `ftruncate_fd` does.
 */
long ksys_ftruncate(int fd, long long length)
{
    int big = fd_change_lock(fd);
    long rc = ftruncate_fd(fd, length);

    big_give(big);
    return rc;
}

/** ftruncate(2) takes fd and length, the length a signed register's worth,
 * so of 32 bits on i686, whose ftruncate64 is in arch/i686/sys_ia32.c; it
 * returns as `ksys_ftruncate` does. */
long sys_ftruncate(isr_params_t *params)
{
    return ksys_ftruncate((int)SC_ARG(params, 0), SC_SIGNED(params, 1));
}

/** write(2) takes fd, buf and the raw count, which write_entry checks
 * before cutting it at Linux's MAX_RW_COUNT; the bytes written, or -errno.
 * The descriptor is looked up once, as Linux's ksys_write looks it up with
 * fdget_pos, and the call holds the big lock unless its mode says its
 * writes run without it (FD_MODE_WRITE_FREE, `rw_call_lock`). */
long sys_write(isr_params_t *params)
{
    fd_entry_t *e = current_proc ? fd_get(current_proc->fds, (int)SC_ARG(params, 0)) : NULL;
    int big = rw_call_lock(e, FD_MODE_WRITE_FREE);
    long rc = write_entry(e, (const char __user *)SC_ARG(params, 1), SC_ARG(params, 2), 1);

    file_ops_unlock(big);
    return rc;
}

/** Element `i` of the vector of struct iovec at `uvec`, in the caller's
 * layout, as Linux's iovec_from_user reads it: two 32-bit words for an i386
 * call on the x86-64 kernel (`in_compat_syscall`), whose length is then never
 * past 0xFFFFFFFF. 0, or -EFAULT. */
int iov_fetch(struct kernel_iovec *iov, uintptr_t uvec, uint32_t i)
{
    if (in_compat_syscall()) {
        uint32_t w[2];

        if (copy_from_user(w, (const void __user *)(uvec + (uintptr_t)i * sizeof(w)), sizeof(w)) !=
            0) {
            return -EFAULT;
        }
        iov->iov_base = w[0];
        iov->iov_len = w[1];
        return 0;
    }
    return copy_from_user(iov, (const void __user *)(uvec + (uintptr_t)i * sizeof(*iov)),
                          sizeof(*iov)) != 0
               ? -EFAULT
               : 0;
}

/**
 * Check the `count` elements of the vector at `uvec` as Linux's
 * __import_iovec does, and return their total, cut at MAX_RW_COUNT
 * (USER_RW_MAX), or -errno. Every length is checked first: one negative as
 * the caller's ssize_t, which for an i386 call (`in_compat_syscall`) is one
 * past 0x7FFFFFFF, is -EINVAL. Then each range must end in user space
 * (`user_span_ok`), its whole length for a vector of several elements and
 * its length cut to USER_RW_MAX for one alone, as Linux's import_ubuf cuts
 * first, or the answer is -EFAULT; the element that crosses USER_RW_MAX is
 * cut there and those after it to 0. With `prefault` 0 or 1 each element's
 * cut bytes must also be readable, or writable, and are faulted in
 * (`user_buffer_ok`), or the answer is -EFAULT, so that a transfer that
 * cannot be undone meets a bad buffer before a byte moves; -1 skips this.
 * With `iov` not NULL, it holds `count` elements and gets them, cut. An
 * unreadable vector is -EFAULT. It may sleep.
 */
long iov_import(struct kernel_iovec *iov, uintptr_t uvec, int count, int prefault)
{
    struct kernel_iovec one;
    uint32_t total = 0;
    int pass, i;

    for (pass = 0; pass < 2; pass++) {
        for (i = 0; i < count; i++) {
            struct kernel_iovec *e = iov ? &iov[i] : &one;
            unsigned long len;

            if ((pass == 0 || !iov) && iov_fetch(e, uvec, (uint32_t)i) != 0) {
                return -EFAULT;
            }
            if ((long)e->iov_len < 0 || (in_compat_syscall() && e->iov_len > 0x7FFFFFFFu)) {
                return -EINVAL;
            }
            if (pass == 0) {
                continue;
            }
            len = count == 1 ? USER_RW_LEN(e->iov_len) : e->iov_len;
            if (!user_span_ok(e->iov_base, len)) {
                return -EFAULT;
            }
            if (len > USER_RW_MAX - total) {
                len = USER_RW_MAX - total;
            }
            if (prefault >= 0 && len > 0 && !user_buffer_ok(e->iov_base, len, prefault)) {
                return -EFAULT;
            }
            e->iov_len = len;
            total += (uint32_t)len;
        }
    }
    return (long)total;
}

/** The bytes of a vector of `count` bytes that a tmpfs file sealed against
 * growth lets in, into `*room`, before any element is written: the whole
 * 2 MiB chunks before its end once the vector would pass it
 * (`tmpfs_grow_room`), as Linux writes a vector as one stream in such
 * chunks. `pos` is its start unless `append` asks for the end. The caller
 * holds the mount's namespace lock or takes the vector element by element.
 * Return 0, -EPERM when no byte fits, or getattr's error. Other file types
 * have no seals, and `*room` is `count` for them. */
static int32_t iov_seal_grow(const struct open_file *ofd, uint64_t pos, uint32_t count, int append,
                             uint32_t *room)
{
    struct vfs_stat st;
    int rc;

    *room = count;
    if (!count || ofd->vmnt->type != &vfs_tmpfs_type ||
        !(tmpfs_seals(ofd->vnode) & TMPFS_SEAL_GROW)) {
        return 0;
    }
    rc = vfs_getattr(ofd->vmnt, ofd->vnode, &st);
    if (rc != 0) {
        return rc;
    }
    if (append) {
        pos = st.size;
    }
    *room = tmpfs_grow_room(pos, count, st.size);
    return *room == 0 ? -EPERM : 0;
}

/**
 * Write the `iovcnt` elements of the vector at `iov_user`, which
 * `iov_import` checked, to `fd`, one at a time by `write_fd`, and return
 * the bytes written. A short write ends the call, as writev does. A fault
 * or a failed write ends the call too: with the error when nothing was
 * written yet, and otherwise with the bytes before it, as a short write
 * does, since a caller that retried after the error would write those bytes
 * again. No more than `cap` bytes are written, the total `iov_import` gave
 * or less, which ends the call as a short write does.
 */
static int32_t writev_elements(int fd, uintptr_t iov_user, int iovcnt, uint32_t cap)
{
    struct kernel_iovec iov;
    int32_t total = 0;
    int i;

    for (i = 0; i < iovcnt && (uint32_t)total < cap; i++) {
        uint32_t len;
        int32_t r;

        if (iov_fetch(&iov, iov_user, (uint32_t)i) != 0) {
            return (total > 0) ? total : -EFAULT;
        }
        if (iov.iov_len == 0) {
            continue;
        }
        len = iov.iov_len < cap - (uint32_t)total ? (uint32_t)iov.iov_len : cap - (uint32_t)total;
        if (!user_access_ok(iov.iov_base, len)) {
            return (total > 0) ? total : -EFAULT;
        }
        r = write_fd(fd, (const char __user *)iov.iov_base, len, total == 0);
        if (r < 0) {
            return (total > 0) ? total : r;
        }
        total += r;
        if ((uint32_t)r < iov.iov_len) {
            break;
        }
    }

    return total;
}

/** Whether a held descriptor is a file of a mount, whose vectors go
 * through `iov_file_write` under one lock. */
static int iov_mount_file(const fd_entry_t *entry)
{
    return entry && entry->type == FD_TYPE_FILE && !entry->ops && entry->ofd && entry->ofd->vmnt;
}

/** Whether a held descriptor selects the mounted-file vector append path. */
static int iov_append_file(const fd_entry_t *entry)
{
    return iov_mount_file(entry) && (entry->ofd->status_flags & OFD_APPEND);
}

/** Write a copied user vector under the file's lock, as defined below. */
static long iov_file_write(fd_entry_t *entry, uintptr_t addr, int count, long long pos, int append);

/** Read a copied user vector under the description's lock, as defined
 * below. */
static long iov_file_read(fd_entry_t *entry, uintptr_t addr, int count, long long pos);

/** Cut an imported vector to the prefix the caller can reach, as defined
 * below. */
static uint32_t iov_prefix(struct kernel_iovec *iov, int count, int into);

/** Import the vector once and write it whole to a pty master, under the
 * pair's lock (`pty_master_writev`), or to a pipe end as one write
 * (`pipe_writev`), or a Unix socket as one send (`unix_writev`).
 * `entry` is held and open for writing, and `count` is in
 * range. Return the bytes written or an import, allocation or write error. */
static long writev_whole(fd_entry_t *entry, uintptr_t addr, int count)
{
    struct kernel_iovec local[8], *iov = local;
    long rc;

    if (count > (int)(sizeof(local) / sizeof(local[0]))) {
        iov = kmalloc((size_t)count * sizeof(*iov));
        if (!iov) {
            return -ENOMEM;
        }
    }
    rc = iov_import(iov, addr, count, -1);
    if (rc > 0) {
        if (entry->type == FD_TYPE_PTM) {
            rc = pty_master_writev(entry, iov, count);
        } else if (entry->type == FD_TYPE_UNIX) {
            rc = unix_writev(entry, iov, count, (uint32_t)rc);
        } else {
            rc = pipe_writev(entry, iov, count);
        }
    }
    if (iov != local) {
        kfree(iov);
    }
    return rc;
}

/**
 * writev(2) takes fd, the vector and its count. A file of a mount is written
 * under its description's lock, held across the vector and the check of the
 * vector's end at the offset, as Linux holds f_pos_lock, and a vector that
 * would grow a sealed tmpfs file is cut to the whole 2 MiB chunks before its
 * end (`iov_file_write`). A pty master imports the vector once and holds its
 * write lock across it, and a pipe end takes it as one write, as Linux's
 * `pipe_write` takes it; a Unix socket sends the vector as one send, as
 * Linux's sock_write_iter does (`writev_whole`). Otherwise elements are
 * written in order (`writev_elements`). The vector is checked whole first
 * (`iov_import`), and its total cut at MAX_RW_COUNT. The descriptor is
 * checked before the count and the vector, as Linux tests FMODE_WRITE
 * first, and a vector of no bytes then returns 0 without a write
 * (`write_check`). Returns the bytes written, or a negative errno: -EBADF
 * or -EINVAL for a descriptor `write_check` refuses, -EINVAL for a count
 * below 0 or past IOV_MAX_LOCAL, a negative length or a total whose end at
 * the description's offset passes 2^63-1 (`rw_offset_end`), -EFAULT for a
 * vector or a range outside user space, -EPERM for a vector of which the
 * seal lets no byte in, or a transfer error when no byte was written. A
 * buffer fault after a positive transfer returns the bytes written before
 * the fault.
 */
static long writev(int fd, uintptr_t iov_user, int iovcnt)
{
    fd_entry_t *entry;
    long wanted;
    int32_t rc;

    rc = write_check(fd);
    if (rc != 0) {
        return rc;
    }
    if (iovcnt < 0 || iovcnt > IOV_MAX_LOCAL) {
        return -EINVAL;
    }
    entry = fd_get(current_proc->fds, fd);
    if (entry && (entry->type == FD_TYPE_PTM || entry->type == FD_TYPE_UNIX || pipe_is(entry))) {
        return writev_whole(entry, iov_user, iovcnt);
    }
    if (iov_mount_file(entry)) {
        return iov_file_write(entry, iov_user, iovcnt, -1,
                              (entry->ofd->status_flags & OFD_APPEND) != 0);
    }
    wanted = iov_import(NULL, iov_user, iovcnt, -1);
    if (wanted <= 0) {
        return wanted;
    }
    rc = rw_offset_end(entry, (size_t)wanted);
    if (rc != 0) {
        return rc;
    }
    return writev_elements(fd, iov_user, iovcnt, (uint32_t)wanted);
}

/**
 * The big lock for readv or writev of descriptor `fd`, held for the call
 * (`fd_get`): none for a pipe end or a Unix socket, whose operations run
 * without it (`file_ops_lock`), as read and write of one do, and taken for
 * every other
 * descriptor, whose vector paths still need it. Returns whether it was
 * taken, for `file_ops_unlock`.
 */
static int vec_call_lock(int fd)
{
    fd_entry_t *e = current_proc ? fd_get(current_proc->fds, fd) : NULL;

    if (e && (pipe_is(e) || e->type == FD_TYPE_UNIX)) {
        return file_ops_lock(e);
    }
    bkl_enter();
    return 1;
}

/** writev(2): the descriptor, vector and count in the caller's registers,
 * under the big lock unless it is a pipe end or a Unix socket
 * (`vec_call_lock`) or a file whose writes run without it
 * (`file_vfs_writes_unlocked`). */
long sys_writev(isr_params_t *params)
{
    int fd = (int)SC_ARG(params, 0);
    int big = file_vfs_writes_unlocked(current_proc ? fd_get(current_proc->fds, fd) : NULL)
                  ? 0
                  : vec_call_lock(fd);
    long rc = writev(fd, SC_ARG(params, 1), (int)SC_ARG(params, 2));

    file_ops_unlock(big);
    return rc;
}

/* Linux open flags */
#define O_APPEND 02000 /* every write lands at end-of-file */

#define O_CREAT 0100

#define O_EXCL 0200

#define O_TRUNC 01000

#define O_NONBLOCK 04000 /* a would-block operation returns -EAGAIN */

#define O_DIRECTORY 0200000 /* the target must be a directory (else -ENOTDIR) */

#define O_NOFOLLOW 0400000 /* a symbolic link at the end answers -ELOOP */

#define O_CLOEXEC 02000000 /* close-on-exec: the fd is dropped across execve */

#define O_PATH 010000000 /* a descriptor that names a file and opens nothing */

/* Special dir fd for the openat family: resolve relative to the current dir. */
#define AT_FDCWD (-100)

#define AT_SYMLINK_NOFOLLOW 0x100

#define AT_SYMLINK_FOLLOW 0x400 /* linkat: follow a symlink source */

#define AT_EMPTY_PATH 0x1000 /* *at: operate on the dirfd itself */

#define AT_EACCESS 0x200 /* faccessat2: judge by the effective ids */

#define AT_NO_AUTOMOUNT 0x800 /* fstatat: accepted and meaningless here */

/* The file type bits of a mode, as st_mode has them. */
#define S_IFMT   0170000
#define S_IFSOCK 0140000
#define S_IFREG  0100000
#define S_IFBLK  0060000
#define S_IFDIR  0040000
#define S_IFCHR  0020000
#define S_IFIFO  0010000

/**
 * Put `base` and a slash in front of the relative path of `len` bytes in
 * `buf`, in place: the join of a directory and a name below it.
 *
 * `base` is "/" or "/a/b" with no trailing slash, and not inside `buf`; the
 * root puts the slash alone in front. Returns the new length, or
 * -ENAMETOOLONG when the whole does not fit `size`.
 */
static int path_prepend(char *buf, uint32_t size, uint32_t len, const char *base)
{
    uint32_t blen = (uint32_t)k_strlen(base);

    if (blen <= 1) {
        blen = 0;
    }
    if (blen + 1 + len + 1 > size) {
        return -ENAMETOOLONG;
    }
    k_memmove(buf + blen + 1, buf, len + 1);
    k_memcpy(buf, base, blen);
    buf[blen] = '/';
    return (int)(blen + 1 + len);
}

/**
 * Whether the working directory has been removed while it was one: its
 * node, which the working directory holds, has no name left, which a type
 * reports as a link count of 0.
 */
static int cwd_removed(void)
{
    struct fs_struct *fs = current_proc->fs;
    struct vfs_stat st;

    return fs && fs->cwd_mnt && fs->cwd_node && vfs_getattr(fs->cwd_mnt, fs->cwd_node, &st) == 0 &&
           st.nlink == 0;
}

/**
 * Take a name off the removed working directory's path, the first `blen`
 * bytes of `path`, for each `..` the relative path after it begins with.
 *
 * The walk of the joined path would fail at the removed directory's name,
 * where Linux climbs from the directory to the parent it had, so `cd ..`
 * leaves a directory removed under the shell. The parent is the one the
 * path names, which is the one it had unless a directory above was renamed.
 */
static void removed_cwd_parents(char *path, uint32_t blen)
{
    const char *rest = path + blen + 1;
    uint32_t keep = blen;

    while (rest[0] == '.' && rest[1] == '.' && (rest[2] == '/' || rest[2] == '\0')) {
        while (keep > 0 && path[keep - 1] != '/') {
            keep--;
        }
        if (keep > 0) {
            keep--;
        }
        rest += 2;
        while (*rest == '/') {
            rest++;
        }
    }
    if (rest != path + blen + 1) {
        path[keep] = '/';
        k_memmove(path + keep + 1, rest, k_strlen(rest) + 1);
    }
}

/**
 * Collapse `.` and `..` in an absolute path that a physical walk has just
 * resolved, for the working directory's string: after the walk succeeded,
 * every `..` names the parent the walk climbed to. `path` must start with
 * '/'; the output never grows, so the rewrite is in place.
 */
static void collapse_dots(char *path)
{
    int out = 1;
    const char *p = path + 1;

    while (*p) {
        const char *comp;
        int clen, i;

        while (*p == '/') {
            p++;
        }
        if (!*p) {
            break;
        }
        comp = p;
        while (*p && *p != '/') {
            p++;
        }
        clen = (int)(p - comp);
        if (clen == 1 && comp[0] == '.') {
            continue;
        }
        if (clen == 2 && comp[0] == '.' && comp[1] == '.') {
            while (out > 1 && path[out - 1] != '/') {
                out--;
            }
            if (out > 1) {
                out--;
            }
            continue;
        }
        if (out > 1) {
            path[out++] = '/';
        }
        for (i = 0; i < clen; i++) {
            path[out++] = comp[i];
        }
    }
    path[out] = '\0';
}

int canonicalize_abs(char *path)
{
    int out = 1;
    const char *p = path + 1;

    while (*p) {
        const char *comp;
        int clen, i;

        while (*p == '/') {
            p++;
        }
        if (!*p) {
            break;
        }
        comp = p;
        while (*p && *p != '/') {
            p++;
        }
        clen = (int)(p - comp);

        if (clen == 1 && comp[0] == '.') {
            continue;
        }
        if (out > 1) {
            path[out++] = '/';
        }
        for (i = 0; i < clen; i++) {
            path[out++] = comp[i];
        }
    }
    path[out] = '\0';
    return out;
}

/**
 * A NULL node is the root directory, or a file of another type, and needs no
 * check. Two shapes of node are live: an orphan, which no path names and which
 * nothing frees while the descriptor holds it; and a named node, which must
 * still resolve back to itself. Anything else, a node no longer reserved or a
 * name that resolves elsewhere or is too long to build, is a descriptor whose
 * file went away by a route that did not count the holders.
 *
 * The check does not tell a freed node from another file the allocator later
 * placed at the same address. The lifetime rule makes that case impossible for
 * every regular-file descriptor: no address a descriptor has is handed out
 * while it has it. A directory is checked the same way, though `rmdir` fails on
 * one a description has: one that fails the check was removed by a route that
 * did not look for its descriptors.
 */
int fd_node_check(fd_entry_t *entry)
{
    char path[256];
    krffs_node_t *node = entry->node;

    if (node == NULL) {
        return 0;
    }
    if (node->type != KRFFS_RESERVED_NODE) {
        return -EBADF;
    }
    if (node->name[0] == '\0') {
        return 0;
    }
    if (krffs_node_path(node, path, sizeof(path)) != 0) {
        return -EBADF;
    }
    if (krffs_resolve(path) != node) {
        return -EBADF;
    }
    return 0;
}

/** Whether `path` is `root`, a path of `rl` bytes, or a path below it. */
static int path_under(const char *path, const char *root, uint32_t rl)
{
    uint32_t i;

    if (rl <= 1) {
        return 1;
    }
    for (i = 0; i < rl; i++) {
        if (path[i] != root[i]) {
            return 0;
        }
    }
    return path[rl] == '/' || path[rl] == '\0';
}

/**
 * The path of the calling process's root from the root of the mount table,
 * into `root`, a path page: "/" for a process chroot has given no root of
 * its own.
 *
 * The root is held while its path is rebuilt from its volume, which may
 * sleep, as `cwd_global` holds it: a thread that shares the directories
 * (CLONE_FS) may chroot meanwhile and let the old root go. Returns 0; -EBUSY
 * for a root on a mount being unmounted; or the error `vfs_node_path` gives
 * for the root.
 */
static int32_t root_global(char *root)
{
    struct vfs_ref r;
    int32_t rc;

    if (!current_proc || !current_proc->fs->root_mnt) {
        root[0] = '/';
        root[1] = '\0';
        return 0;
    }
    r.mnt = current_proc->fs->root_mnt;
    r.node = current_proc->fs->root_node;
    if (vfs_enter(r.mnt) != 0) {
        return -EBUSY;
    }
    vfs_ref_hold(&r);
    rc = vfs_node_path(r.mnt, r.node, root, VFS_PATH_MAX);
    vfs_ref_release(&r);
    return rc;
}

/**
 * Make `path`, a path from the root of the mount table, the path the calling
 * process has for the same place from the root chroot gave it: the root's
 * own path (`root_global`) comes off the front, and the root itself is "/".
 * A process with no root of its own keeps `path` as it is. Returns 0;
 * -ENOENT for a place outside the process's root, which no path of the
 * process reaches; -ENOMEM; or `root_global`'s error.
 */
static int32_t path_in_root(char *path)
{
    char *root;
    uint32_t rl = 0;
    int32_t rc;

    if (!current_proc || !current_proc->fs->root_mnt) {
        return 0;
    }
    root = path_buf_get();
    if (!root) {
        return -ENOMEM;
    }
    rc = root_global(root);
    if (rc == 0) {
        rl = (uint32_t)k_strlen(root);
        rc = path_under(path, root, rl) ? 0 : -ENOENT;
    }
    if (rc == 0 && rl > 1) {
        k_memmove(path, path + rl, k_strlen(path + rl) + 1);
        if (path[0] == '\0') {
            path[0] = '/';
            path[1] = '\0';
        }
    }
    path_buf_put(root);
    return rc;
}

/** `path_in_root`, but for a place outside the process's root, when
 * `outside` is not NULL, `path` stays as it is and `*outside` is set. */
static int32_t path_in_root_at(char *path, int *outside)
{
    int32_t rc = path_in_root(path);

    if (rc == -ENOENT && outside) {
        *outside = 1;
        rc = 0;
    }
    return rc;
}

/**
 * Whether the canonical path `path` is "/" and names the root of the mount
 * table, which it does for a process chroot has not given a root of its
 * own; the stat calls report that directory without a walk.
 */
static int names_tree_root(const char *path)
{
    return path[0] == '/' && path[1] == '\0' && (!current_proc || !current_proc->fs->root_mnt);
}

/**
 * Whether the working directory's path `cwd` is outside the process's root:
 * it then begins with two slashes and is its path from the root of the mount
 * table, as fchdir to a descriptor opened before a chroot or a pivot_root
 * leaves it (`sys_fchdir`). A path the walk takes with two slashes starts at
 * the root of the mount table.
 */
static int cwd_outside(const char *cwd)
{
    return cwd[0] == '/' && cwd[1] == '/';
}

/**
 * The absolute path of the directory a descriptor is open on, into `out` of
 * `cap` bytes: what its `*at` calls resolve relative names against, and what
 * `fchdir` makes the working directory.
 *
 * A synthetic directory's path is rebuilt from its kind and pid. A directory
 * of a mounted type is named by the type, so the descriptor follows a rename
 * of it, behind the path of the directory the mount covers; a type without a
 * `path` operation keeps the path the directory was opened by. A directory of
 * the KRFFS boot root is named by its node (`entry->node`, NULL for the root
 * itself), which must still resolve back to itself: KRFFS nodes are not
 * counted, and though `rmdir` fails on a directory a description has, the
 * test does not rest on that. A path from the root of the mount table
 * becomes the one the calling process has from its own root
 * (`path_in_root`). A directory outside that root keeps its path from the
 * root of the mount table, with `*outside` set, when `outside` is not NULL.
 * Returns 0, -ENOTDIR for a descriptor on anything but a directory, -EBADF
 * for a boot-root directory that no longer resolves to its node, -ENOENT for
 * one outside the process's root without `outside` or one ext2 or tmpfs
 * removed, or -ENAMETOOLONG.
 */
static int fd_dir_path(fd_entry_t *entry, char *out, uint32_t cap, int *outside)
{
    krffs_node_t *dnode = entry->node;
    int r;

    if (entry->type == FD_TYPE_SYNTH_DIR) {
        struct synth_dir *sd = (struct synth_dir *)entry->priv;

        if (!sd) {
            return -EBADF;
        }
        if (entry->ofd && entry->ofd->dirpath) {
            if (k_strlen(entry->ofd->dirpath) + 1 > cap) {
                return -ENAMETOOLONG;
            }
            k_memcpy(out, entry->ofd->dirpath, k_strlen(entry->ofd->dirpath) + 1);
        } else {
            synth_dir_path(sd->kind, sd->pid, sd->tid, out, (int)cap);
        }
        return path_in_root_at(out, outside);
    }
    if (entry->type != FD_TYPE_DIR) {
        return -ENOTDIR;
    }
    if (!dnode && entry->ofd && entry->ofd->vmnt && !mnt_is_root(entry->ofd->vmnt)) {
        struct vfs_mount *vm = entry->ofd->vmnt;

        if (vm->type->ops->path) {
            r = vfs_node_path(vm, entry->ofd->vnode, out, cap);
            return r != 0 ? r : path_in_root_at(out, outside);
        }
        if (!entry->ofd->dirpath || k_strlen(entry->ofd->dirpath) + 1 > cap) {
            return -ENAMETOOLONG;
        }
        k_memcpy(out, entry->ofd->dirpath, k_strlen(entry->ofd->dirpath) + 1);
        return 0;
    }
    if (dnode) {
        if (dnode->type != KRFFS_RESERVED_NODE) {
            return -EBADF;
        }
        if (krffs_node_path(dnode, out, cap) != 0) {
            return -ENAMETOOLONG;
        }
        if (krffs_resolve(out) != dnode) {
            return -EBADF;
        }
        return path_in_root_at(out, outside);
    }
    out[0] = '/';
    out[1] = '\0';
    return path_in_root_at(out, outside);
}

/* How a path as written names a directory by its end, which its canonical
 * copy no longer shows (`path_names_dir`). */
#define PATH_END_SLASH  1 /* a slash after a name, "d/" */
#define PATH_END_DOT    2 /* a last component ".", "d/." */
#define PATH_END_DOTDOT 3 /* a last component "..", "d/.." */

/**
 * How the path `raw` of `len` bytes names a directory by its end: by a
 * slash after its last name, PATH_END_SLASH, or by a last component of "."
 * or "..", with or without slashes after it, PATH_END_DOT or
 * PATH_END_DOTDOT; 0 when it does not. Its last name is then a directory the
 * walk reaches through a symbolic link, as Linux's does, where a call that
 * does not follow a final link would stop at the link itself. The path "/"
 * is PATH_END_SLASH.
 */
static int path_names_dir(const char *raw, int len)
{
    int end = len;
    int start;

    while (end > 0 && raw[end - 1] == '/') {
        end--;
    }
    start = end;
    while (start > 0 && raw[start - 1] != '/') {
        start--;
    }
    if (end - start == 1 && raw[start] == '.') {
        return PATH_END_DOT;
    }
    if (end - start == 2 && raw[start] == '.' && raw[start + 1] == '.') {
        return PATH_END_DOTDOT;
    }
    return end < len ? PATH_END_SLASH : 0;
}

/**
 * Put back the last "." of a path that named a directory by it, which the
 * canonical `path` of `len` bytes has dropped (`path_names_dir`), and return
 * the new length. The walk then looks "." up in the directory before it, as
 * Linux's walk does, which takes search permission there and answers
 * -ENOTDIR for a name that is not a directory, and a walk to the parent
 * returns "." as the last name, which the calls that make or remove a name
 * answer for. The bytes the canonical path dropped leave room for the two
 * put back.
 */
static int path_put_dot(char *path, int len)
{
    if (len > 1) {
        path[len++] = '/';
    }
    path[len++] = '.';
    path[len] = '\0';
    return len;
}

/** Take off the absolute path `path` the last "." `path_put_dot` put back,
 * when `end` says it named a directory by one, for a call that keeps the
 * path as a name. */
static void path_drop_dot(char *path, int end)
{
    uint32_t len = (uint32_t)k_strlen(path);

    if (end == PATH_END_DOT && len >= 2) {
        path[len > 2 ? len - 2 : 1] = '\0';
    }
}

/**
 * Copy a user path into `pathbuf` as an absolute, canonical path, a
 * relative one joined to the directory `dirfd` is open on, or to the working
 * directory for `AT_FDCWD`, and say in `*dir`, when `dir` is not NULL, how
 * the path as written named a directory by its end (`path_names_dir`), which
 * the canonical path no longer shows, or 0; a last "." is then put back
 * (`path_put_dot`).
 *
 * The join is what lets the resolver, which walks from the root, walk the
 * right tree. `size` is at most `VFS_PATH_MAX`: the path the caller gives may
 * be that long with its terminator, and so may the joined one. Returns the
 * length, or a negative errno: -EFAULT for a pointer outside the caller's
 * memory, -ENOENT for the empty path, which names no file (a call that takes
 * AT_EMPTY_PATH looks for it before copying), -ENAMETOOLONG for either path
 * longer than the buffer, and for a relative path only, since an absolute
 * one does not read `dirfd`, -EBADF for a descriptor that is not open,
 * -ENOTDIR for one that is not a directory, -ENOMEM when the heap has no
 * path page, or another error of `fd_dir_path`.
 *
 * A relative path joined to a directory outside the process's root, which
 * a descriptor opened before a chroot or a pivot_root may be on, and so the
 * working directory an fchdir to one gives, begins with three slashes,
 * and still with two once `path_name` takes the first off: the walk starts
 * a path that begins with two at the root of the mount table, as Linux
 * walks from such a descriptor, and a symbolic link's absolute target and
 * `..` at the root still go by the process's root (fs/vfs.c). A path the
 * caller wrote never begins so, since the canonical path has one slash.
 */
static int copy_path_at(char *pathbuf, uint32_t size, int dirfd, const char __user *upath, int *dir)
{
    fd_entry_t *entry;
    char *dirpath;
    int len, r = 0, outside = 0;

    len = strncpy_from_user(pathbuf, upath, size);
    if (len < 0) {
        return len;
    }
    if (len == 0) {
        return -ENOENT;
    }
    if (dir) {
        *dir = path_names_dir(pathbuf, len);
    }
    if (pathbuf[0] != '/' && dirfd == AT_FDCWD) {
        int dotdot = pathbuf[0] == '.' && pathbuf[1] == '.';

        outside = cwd_outside(proc_cwd(current_proc));
        r = path_prepend(pathbuf, size, (uint32_t)len, proc_cwd(current_proc));
        if (r >= 0 && dotdot && cwd_removed()) {
            removed_cwd_parents(pathbuf, (uint32_t)(r - len - 1));
        }
    } else if (pathbuf[0] != '/') {
        entry = fd_get(current_proc->fds, dirfd);
        if (!entry) {
            return -EBADF;
        }
        dirpath = path_buf_get();
        if (!dirpath) {
            return -ENOMEM;
        }
        r = fd_dir_path(entry, dirpath, VFS_PATH_MAX, &outside);
        if (r == 0) {
            r = path_prepend(pathbuf, size, (uint32_t)len, dirpath);
        }
        path_buf_put(dirpath);
    }
    if (r < 0) {
        return r;
    }
    len = canonicalize_abs(pathbuf);
    if (outside) {
        if ((uint32_t)len + 4 > size) {
            return -ENAMETOOLONG;
        }
        k_memmove(pathbuf + 2, pathbuf, (uint32_t)len + 1);
        pathbuf[1] = '/';
        len += 2;
    }
    return (dir && *dir == PATH_END_DOT) ? path_put_dot(pathbuf, len) : len;
}

/**
 * The path a caller named, in a path page: absolute, canonical, and joined to
 * the directory `dirfd` is open on when it is relative, or to the working
 * directory for `AT_FDCWD`; with `dir` not NULL, how it named a directory by
 * its end, and with a last "." put back, as `copy_path_at` says.
 *
 * Returns 0 with the page in `*out`, which the caller gives back with
 * `path_buf_put`, or a negative errno with nothing taken: -ENOMEM when the
 * heap has no page, or `copy_path_at`'s.
 */
static int path_from_user_dir(int dirfd, const char __user *upath, char **out, int *dir)
{
    char *buf = path_buf_get();
    int len;

    if (!buf) {
        return -ENOMEM;
    }
    len = copy_path_at(buf, VFS_PATH_MAX, dirfd, upath, dir);
    if (len < 0) {
        path_buf_put(buf);
        return len;
    }
    *out = buf;
    return 0;
}

/* The answer of a call's path without the big lock when the call must run
 * again under it, with nothing done that a caller sees: below every errno,
 * whose least is -MAX_ERRNO (Linux's include/linux/err.h:18), and every
 * descriptor, since `open_unlocked` returns a descriptor on success and
 * Linux's do_sys_openat2 returns the one it installs (fs/open.c:1419-1430). */
#define CALL_LOCKED (-4095 - 1)

/**
 * The path a caller named, as `path_from_user_dir` makes it, for a call
 * without the big lock: an absolute path, or one relative to the working
 * directory, whose path is copied under its `fs_struct`'s lock
 * (`proc_cwd_read`). 0 with the page in `*out`; a negative errno of
 * `copy_path_at`'s; or CALL_LOCKED with nothing taken, for a path relative
 * to a descriptor, whose path the type gives, one that begins with "..",
 * which a removed working directory answers, or a working directory
 * outside the process's root.
 */
static int path_from_user_unlocked(int dirfd, const char __user *upath, char **out, int *dir)
{
    char *buf = path_buf_get(), *cwd = NULL;
    int len, locked = 0;

    if (!buf) {
        return -ENOMEM;
    }
    len = strncpy_from_user(buf, upath, VFS_PATH_MAX);
    if (len >= 0 && dir) {
        *dir = path_names_dir(buf, len);
    }
    if (len > 0 && buf[0] != '/') {
        cwd = dirfd == AT_FDCWD && !(buf[0] == '.' && buf[1] == '.') ? path_buf_get() : NULL;
        if (cwd) {
            proc_cwd_read(current_proc, cwd);
        }
        locked = !cwd || cwd_outside(cwd);
        if (!locked) {
            len = path_prepend(buf, VFS_PATH_MAX, (uint32_t)len, cwd);
        }
        path_buf_put(cwd);
    }
    if (len > 0 && !locked) {
        len = canonicalize_abs(buf);
        if (dir && *dir == PATH_END_DOT) {
            len = path_put_dot(buf, len);
        }
    }
    if (len <= 0 || locked) {
        path_buf_put(buf);
        return locked ? CALL_LOCKED : len == 0 ? -ENOENT : len;
    }
    *out = buf;
    return 0;
}

/** `path_from_user_dir` for a caller that does not ask how the path ends. */
static int path_from_user(int dirfd, const char __user *upath, char **out)
{
    return path_from_user_dir(dirfd, upath, out, NULL);
}

int copy_path_cwd(char *pathbuf, uint32_t size, const char __user *upath, int *end)
{
    return copy_path_at(pathbuf, size, AT_FDCWD, upath, end);
}

int copy_path_exec_at(char *pathbuf, uint32_t size, int dirfd, const char __user *upath, int *end)
{
    return copy_path_at(pathbuf, size, dirfd, upath, end);
}

int canonicalize_named(char *path, int *end)
{
    int len;

    *end = path_names_dir(path, (int)k_strlen(path));
    len = canonicalize_abs(path);
    return *end == PATH_END_DOT ? path_put_dot(path, len) : len;
}

/** Whether `leaf`, the last name a walk to its parent gave, is "." or "..",
 * which a path names only as a directory, and no call makes or removes. */
static int leaf_is_dot(const char *leaf)
{
    return leaf[0] == '.' && (leaf[1] == '\0' || (leaf[1] == '.' && leaf[2] == '\0'));
}

/** The absolute path `abs` without its leading slash, as the cores take it. */
static const char *path_name(const char *abs)
{
    return abs[0] == '/' ? abs + 1 : abs;
}

/** Whether the string `s` begins with `prefix`. */
static int path_starts(const char *s, const char *prefix)
{
    while (*prefix != '\0' && *s == *prefix) {
        s++;
        prefix++;
    }
    return *prefix == '\0';
}

/**
 * The length of the first part of the absolute path `path` that names a
 * link the walk follows to its object, /proc/PID/fd/N, cwd or root, or
 * /dev/fd/N, with more of the path after it, and in `vp` the link; 0 when
 * the path has none. Only a path under /proc or /dev can.
 */
static uint32_t path_object_link(char *path, struct vpath *vp)
{
    uint32_t at = 1;

    if (!path_starts(path + 1, "proc/") && !path_starts(path + 1, "dev/")) {
        return 0;
    }
    for (;;) {
        uint32_t next;
        char was;

        while (path[at] != '\0' && path[at] != '/') {
            at++;
        }
        for (next = at; path[next] == '/'; next++) {
        }
        if (path[next] == '\0') {
            return 0;
        }
        was = path[at];
        path[at] = '\0';
        synth_classify(path, vp);
        path[at] = was;
        if (vp->kind == VNODE_PROC_FD_LINK || vp->kind == VNODE_PROC_CWD ||
            vp->kind == VNODE_PROC_ROOT) {
            return at;
        }
        at = next;
    }
}

/**
 * Replace in `path`, an absolute path in a path page, each link the walk
 * followed to its object (`path_object_link`) with the object's path when
 * the walk was made, and collapse the dots after it, as the physical walk
 * from the object went. A directory the caller has open is named as its
 * `*at` calls name it (`fd_dir_path`), which follows a rename on a type that
 * can name its directories; any other object by the link's text. A path a
 * description or a working directory records then names the object, and not
 * a descriptor of the process that walked it, which may be closed or reused.
 * A link whose text is no absolute path, a process gone, and a result too
 * long for the page leave the rest of `path` as it is.
 */
static void path_expand_links(char *path)
{
    struct vpath vp;
    char *text = NULL;
    uint32_t at;
    int n, links;

    for (links = 0; links < VFS_LINK_MAX; links++) {
        process_t *p;
        int pid;

        at = path_object_link(path, &vp);
        if (at == 0) {
            break;
        }
        if (!text && !(text = path_buf_get())) {
            break;
        }
        pid = procfs_resolve_id(&vp);
        p = pid < 0 ? NULL : get_process((uint32_t)pid);
        if (!p || p->state == PROC_UNUSED) {
            break;
        }
        if (vp.kind == VNODE_PROC_FD_LINK) {
            fd_entry_t *own = p->fds == current_proc->fds ? fd_get(p->fds, vp.procfd) : NULL;

            if (own && (own->type == FD_TYPE_DIR || own->type == FD_TYPE_SYNTH_DIR)) {
                n = fd_dir_path(own, text, VFS_PATH_MAX, NULL) == 0 ? (int)k_strlen(text) : -1;
            } else {
                n = procfs_readlink_fd(pid, vp.procfd, text, VFS_PATH_MAX);
            }
        } else {
            n = procfs_readlink_dir(pid, vp.kind == VNODE_PROC_ROOT, text, VFS_PATH_MAX - 1);
        }
        if (n <= 0 || text[0] != '/' || (uint32_t)n + k_strlen(path + at) + 1 > VFS_PATH_MAX) {
            break;
        }
        k_memmove(path + n, path + at, k_strlen(path + at) + 1);
        k_memcpy(path, text, (uint32_t)n);
        collapse_dots(path);
    }
    path_buf_put(text);
}

/**
 * Record `name`, the path a description was opened by with its leading '/'
 * stripped, in `ofd`, for /proc/PID/fd to show and for a directory of a type
 * without `path` to resolve names against: with the links of /proc it went
 * through replaced by the paths of their objects (`path_expand_links`),
 * without the last "." that `path_put_dot` put back, in a copy from the heap
 * that replaces the one `ofd` had, and none when the heap has no room.
 */
static void ofd_record_path(struct open_file *ofd, const char *name)
{
    char *full = NULL;
    uint32_t len = (uint32_t)k_strlen(name);

    if ((path_starts(name, "proc/") || path_starts(name, "dev/")) && len + 2 <= VFS_PATH_MAX &&
        (full = path_buf_get()) != NULL) {
        full[0] = '/';
        k_memcpy(full + 1, name, len + 1);
        path_expand_links(full);
        name = full + 1;
        len = (uint32_t)k_strlen(name);
    }

    if (len > 0 && name[len - 1] == '.' && (len == 1 || name[len - 2] == '/')) {
        len = len > 1 ? len - 2 : 0;
    }
    kfree(ofd->dirpath);
    ofd->dirpath = kmalloc(len + 2);
    if (ofd->dirpath) {
        ofd->dirpath[0] = '/';
        k_memcpy(ofd->dirpath + 1, name, len);
        ofd->dirpath[len + 1] = '\0';
    }
    path_buf_put(full);
}

static int32_t synth_open_dir(const struct vpath *vp, int flags);
static int synth_kind_is_dir(int kind);
static uint32_t synth_pid_dir_mode(const struct vpath *vp);

/** Whether `m` is one of the permanent mounts (VFS_PERMANENT), the system's
 * /dev, /proc and /sys among them. */
static int mount_permanent_at(const struct vfs_mount *m)
{
    int i;

    for (i = 0; i < VFS_PERMANENT; i++) {
        if (vfs_mount_at(i) == m) {
            return 1;
        }
    }
    return 0;
}

/** Keep the mount of a synthetic open for later descriptor-link checks.
 * `ref` remains held by the caller. A followed magic link keeps its
 * object's mount instead; namespace objects have no file-system mount. */
static void synth_mount_record(int fd, const struct vfs_ref *ref, const struct vpath *vp, int flags)
{
    struct open_file *ofd;
    struct vfs_ref root;

    if (fd < 0 || ((vp->kind == VNODE_PROC_FD_LINK || vp->kind == VNODE_PROC_NS_LINK) &&
                   !(flags & O_NOFOLLOW))) {
        return;
    }
    ofd = fd_slot(current_proc->fds, fd)->ofd;
    if (!ofd || ofd->vmnt || ofd->synth_mnt) {
        return;
    }
    root.mnt = ref->mnt;
    root.node = root.mnt->root;
    vfs_ref_hold(&root);
    vfs_desc_open(root.mnt);
    ofd->synth_mnt = root.mnt;
}

/**
 * Record `name`, a path as `ofd_record_path` takes it, in the description of
 * `fd`, a descriptor the open of a synthetic node made, when it is open on a
 * directory that the walk reached through a mount other than the system's
 * /dev, /proc and /sys (`permanent` 0): a bind of one of them or a proc
 * mounted elsewhere, as in a sandbox. Its link in /proc/PID/fd then names it
 * where the caller reached it, which bubblewrap reads back to find the mount
 * it made in mountinfo, and a name relative to it stays below that mount
 * (`fd_dir_path`). A directory of the system's own keeps the path its kind
 * gives.
 *
 * The path is kept from the root of the mount table, as a synthetic
 * directory's own path is, so that a chroot after the open does not move it:
 * `name` from the process's root gets the root's path in front, and one that
 * begins with a slash, from the root of the mount table as `copy_path_at`
 * makes a path outside the root, loses that slash. A path that cannot be
 * made so leaves the directory the path its kind gives.
 */
static void synth_dir_record(int fd, int permanent, const char *name)
{
    fd_entry_t *e = fd >= 0 ? fd_slot(current_proc->fds, fd) : NULL;
    char *root, *full;
    uint32_t rl, dl;

    if (!e || permanent || e->type != FD_TYPE_SYNTH_DIR || !e->ofd) {
        return;
    }
    ofd_record_path(e->ofd, name[0] == '/' ? name + 1 : name);
    if (!e->ofd->dirpath || name[0] == '/' || !current_proc->fs->root_mnt) {
        return;
    }
    root = path_buf_get();
    full = NULL;
    if (root && root_global(root) == 0) {
        rl = (uint32_t)k_strlen(root);
        dl = (uint32_t)k_strlen(e->ofd->dirpath);
        if (rl == 1) {
            path_buf_put(root);
            return;
        }
        if (rl + dl < VFS_PATH_MAX && (full = kmalloc(rl + dl + 1)) != NULL) {
            k_memcpy(full, root, rl);
            k_memcpy(full + rl, e->ofd->dirpath, dl + 1);
            if (dl == 1) {
                full[rl] = '\0';
            }
        }
    }
    path_buf_put(root);
    kfree(e->ofd->dirpath);
    e->ofd->dirpath = full;
}

/** Whether a synthetic directory of `kind` is one of a process's, whose
 * process must be there and whose record keeps its pid: /proc/<pid> and its
 * `fd`, `fdinfo`, `task` and `ns`. */
static int synth_dir_of_proc(int kind)
{
    return kind == VNODE_PROC_PID || kind == VNODE_PROC_FD_DIR || kind == VNODE_PROC_FDINFO_DIR ||
           kind == VNODE_PROC_TASK_DIR || kind == VNODE_PROC_NS_DIR;
}

static int32_t open_path(const char *name, int flags);
static int32_t proc_fd_open_path(const struct vpath *vp, int flags);

static int32_t open_named(const char *name, int flags, uint32_t mode, int end);

int cred_privileged(void)
{
    return proc_privileged(current_proc);
}

int cred_fs_privileged(void)
{
    return proc_fs_privileged(current_proc);
}

/**
 * Setuid always goes: a file whose contents or owner just changed no longer
 * grants what its owner meant it to grant. Setgid goes only when the
 * group-execute bit is set with it, matching the grant rule in `execve`:
 * without that bit the file cannot hand out a gid, and the pair means the
 * mandatory-lock mark elsewhere in Unix.
 *
 * The root directory is skipped by node identity, since `KRFFS_IS_DIR` reads
 * false on it: `mkfs` stamps the root a bare 0755 with no type bits, and
 * `KRFFS_MODE_DIRTY` shares the setuid position. A clear that reached the root
 * would report an unclean image as cleanly unmounted, which corrupts the file
 * system, and `chown(path, -1, -1)` is permitted to every caller.
 */
void cred_kill_setid(krffs_node_t *node)
{
    if (!node || node == krffs_fs->node || KRFFS_IS_DIR(node->mode)) {
        return;
    }
    node->mode &= (uint16_t)~KRFFS_MODE_SUID;
    if (node->mode & 0010u) {
        node->mode &= (uint16_t)~KRFFS_MODE_SGID;
    }
}

void vfs_kill_setid(struct vfs_mount *mnt, void *node)
{
    struct vfs_stat vst;
    int big;

    if (!mnt || !node || vfs_getattr(mnt, node, &vst) != 0) {
        return;
    }
    if ((vst.mode & 0170000u) == 0040000u || !(vst.mode & 06000u)) {
        return;
    }
    vst.mode &= ~04000u;
    if (vst.mode & 0010u) {
        vst.mode &= ~02000u;
    }
    big = big_take();
    (void)vfs_setattr(mnt, node, VFS_SET_MODE, &vst);
    big_give(big);
}

/** Non-zero when `gid` is in the caller's supplementary list. */
static int cred_in_suppl(uint32_t gid)
{
    uint32_t i;

    for (i = 0; i < current_proc->ngroups; i++) {
        if (current_proc->groups[i] == gid) {
            return 1;
        }
    }
    return 0;
}

/** Non-zero when `gid` is the caller's file-system gid or in its
 * supplementary list: the set a file owner may chgrp to, and the group the
 * access check matches. */
static int cred_in_group(uint32_t gid)
{
    return gid == proc_fsgid(current_proc) || cred_in_suppl(gid);
}

/**
 * The nine-bit permission check over explicit metadata: 0 when `mode`
 * grants the caller the `want` bits (CRED_R, CRED_W, CRED_X), else -EACCES.
 * One identity class is chosen and judged alone, the classic rule. The owner
 * is judged by the owner bits, else a group match (primary or supplementary)
 * by the group bits, else the other bits; a uid of 0 passes everything,
 * unless the thread gave up its capabilities (`nocaps`). The ids are the
 * file-system ones (`proc_fsuid`, `proc_fsgid`), which are the effective
 * ones unless setfsuid or setfsgid set them apart. `use_real` judges by the
 * real ids instead, for access(2), the one call that asks who the process
 * really is, not who it is acting as.
 */
static int32_t cred_access_bits(uint32_t owner, uint32_t group, uint32_t mode, int want,
                                int use_real)
{
    process_t *p = current_proc;
    uint32_t uid = use_real ? p->ruid : proc_fsuid(p);
    uint32_t bits;

    if (uid == 0 && !p->nocaps) {
        return 0;
    }
    if (uid == owner) {
        bits = mode >> 6;
    } else if ((use_real ? p->rgid : proc_fsgid(p)) == group || cred_in_suppl(group)) {
        bits = mode >> 3;
    } else {
        bits = mode;
    }
    return ((uint32_t)want & ~bits & 7u) ? -EACCES : 0;
}

int mnt_is_root(const struct vfs_mount *mnt)
{
    return mnt == vfs_root_mount() && mnt->type == &vfs_krffs_type;
}

/** Find the current boot-root node of a file holder through any bind alias. */
krffs_node_t *fd_root_node(const fd_entry_t *e)
{
    const struct open_file *ofd = e->ofd;

    if (e->type != FD_TYPE_FILE) {
        return NULL;
    }
    if (ofd && ofd->vmnt && vfs_mount_base(ofd->vmnt) == vfs_root_mount() &&
        ofd->vmnt->type == &vfs_krffs_type) {
        return ofd->vnode != krffs_fs->node ? ofd->vnode : NULL;
    }
    return e->node;
}

int32_t cred_access_ref(const struct vfs_ref *r, int want, int use_real)
{
    struct vfs_stat st;
    int rc = vfs_getattr(r->mnt, r->node, &st);

    if (rc != 0) {
        return rc;
    }
    if ((want & CRED_X) && (st.mode & 0170000u) != 0040000u && !(st.mode & 0111u)) {
        return -EACCES;
    }
    if ((want & CRED_W) && st.flags_known && (st.flags & VFS_FL_IMMUTABLE)) {
        return -EPERM;
    }
    return cred_access_bits(st.uid, st.gid, st.mode & 0777u, want, use_real);
}

/** Whether the inode flags of `node` on `mnt` forbid a change of its size,
 * immutable or append-only, as Linux's do_truncate answers -EPERM; never for
 * a node of the KRFFS root, which keeps no such flags. */
static int size_guarded(struct vfs_mount *mnt, void *node)
{
    struct vfs_stat st;

    return !mnt_is_root(mnt) && vfs_getattr(mnt, node, &st) == 0 && st.flags_known &&
           (st.flags & (VFS_FL_IMMUTABLE | VFS_FL_APPEND)) != 0;
}

/** Whether the file of the description `ofd` is append-only (chattr +a),
 * as Linux's IS_APPEND answers; never for one off a mount or on the KRFFS
 * root, which keep no such flag. */
static int ofd_append_only(const struct open_file *ofd)
{
    struct vfs_stat st;

    return ofd && ofd->vmnt && ofd->vnode && !mnt_is_root(ofd->vmnt) &&
           vfs_getattr(ofd->vmnt, ofd->vnode, &st) == 0 && st.flags_known &&
           (st.flags & VFS_FL_APPEND) != 0;
}

int32_t cred_access_node(const krffs_node_t *node, int want, int use_real)
{
    uint32_t mode = (node == krffs_fs->node) ? 0755u : (uint32_t)(node->mode & 0777u);

    if ((want & CRED_X) && !KRFFS_IS_DIR(node->mode) && node != krffs_fs->node && !(mode & 0111u)) {
        return -EACCES;
    }
    return cred_access_bits(node->uid, node->gid, mode, want, use_real);
}

/**
 * The sticky rule on removing or renaming `victim` out of directory `dir`.
 *
 * With the directory's sticky bit set, write permission on it is no longer
 * enough: only the entry's owner, the directory's owner, or a privileged caller
 * may take an entry out. That is what makes a shared 1777 directory usable, and
 * `-EPERM` is the errno POSIX names for it, not `EACCES`.
 *
 * The SVR4 relaxation that also permits it when the caller can write the file
 * is not POSIX and is not adopted here: it would let a mode a victim chose for
 * their own file decide who may delete it.
 *
 * The root is non-sticky by node identity, not by reading its mode, the
 * same fixed-mode rule `cred_access_node` uses, so `KRFFS_MODE_DIRTY` never
 * enters a permission answer.
 */
static int32_t cred_sticky_ok(const krffs_node_t *dir, const krffs_node_t *victim)
{
    if (!dir || dir == krffs_fs->node || !(dir->mode & KRFFS_MODE_STICKY)) {
        return 0;
    }
    if (cred_fs_privileged() || proc_fsuid(current_proc) == dir->uid ||
        (victim && proc_fsuid(current_proc) == victim->uid)) {
        return 0;
    }
    return -EPERM;
}

static int open_special(struct vfs_ref *ref, int fd, int flags, int32_t *result);
static int open_mounted_node(struct vfs_ref *ref, int flags);
static int open_root_node(struct vfs_ref *ref, int flags);
static int32_t open_install(struct vfs_ref *ref, int fd, const char *name, int flags);

/**
 * Open again the file or directory of a mounted type, or of the KRFFS root,
 * that the description `src` is open on, as an open(2) of it by name with
 * `flags` would, and with the same checks, but found by its node, so that a
 * file that has lost its name opens too. The caller holds `src`. As open(2)
 * does, the descriptor is reserved first, so that no truncation is done for
 * an open that then finds no slot; the node is read from `src` under the
 * mount's namespace lock, where a relocation of the KRFFS root moves it; and
 * a FIFO, a socket or a device file is opened as `open_special` opens one,
 * for a description made by O_PATH. The description is a new one. Returns
 * the new descriptor, or a negative errno: -ENOENT for a mount being
 * unmounted, or the open's.
 */
static int32_t proc_fd_reopen(const struct open_file *src, int flags)
{
    struct vfs_mount *mnt = src->vmnt;
    struct vfs_ref ref;
    int32_t rc;
    int fd = fd_alloc(current_proc->fds);

    if (fd < 0) {
        return fd;
    }
    if (vfs_enter(mnt) != 0) {
        fd_free(current_proc->fds, fd);
        return -ENOENT;
    }
    if (mnt->type->ns_serialised) {
        vfs_ns_lock(mnt);
    }
    ref.mnt = mnt;
    ref.node = src->vnode;
    vfs_ref_hold(&ref);
    if (open_special(&ref, fd, flags, &rc)) {
        return rc;
    }
    rc = mnt_is_root(mnt) ? open_root_node(&ref, flags) : open_mounted_node(&ref, flags);
    if (rc != 0) {
        vfs_walk_unlock(mnt);
        fd_free(current_proc->fds, fd);
        return rc;
    }
    fd_slot(current_proc->fds, fd)->ofd->name = vfs_name_ref(src->name);
    rc = open_install(&ref, fd, src->dirpath ? src->dirpath + 1 : "", flags);
    vfs_walk_unlock(mnt);
    return rc;
}

/**
 * Fill `dst`, a slot the caller reserved, with a copy of the open
 * descriptor `src`, which the caller's call holds: the same open file
 * description, with a reference, which takes the place of any the
 * reservation made, the object behind it with its vtable, a socket's
 * reference, and `src`'s flags with FD_CLOEXEC as `cloexec` says and none
 * of its closing state. The caller then publishes `dst` (`fd_install`).
 */
static void dup_fill(fd_entry_t *dst, const fd_entry_t *src, int cloexec)
{
    fd_ofd_unref(dst->ofd);
    dst->type = src->type;
    dst->node = src->node;
    dst->sock = src->sock;
    dst->ops = src->ops;
    dst->priv = src->priv;
    dst->dev = src->dev;
    dst->ofd = src->ofd;
    fd_ofd_ref(dst->ofd);
    dst->flags = (src->flags & ~(uint32_t)(FD_FLAG_CLOEXEC | FD_FLAG_CLOSING | FD_FLAG_RELEASING)) |
                 (cloexec ? FD_FLAG_CLOEXEC : 0u);
    if (src->type == FD_TYPE_SOCKET && src->sock) {
        socket_ref(src->sock);
    }
}

/* `proc_fd_share`'s flag for an O_PATH descriptor, above every O_ flag. */
#define PROC_FD_SHARE_PATH 0x40000000

/**
 * A new descriptor that shares the description of descriptor `procfd` of
 * the table `fds`, as dup(2) shares it, close-on-exec where `flags` has
 * O_CLOEXEC, and an O_PATH descriptor where it has PROC_FD_SHARE_PATH. The
 * source is looked up again once the slot is had, under its table's lock
 * and with a reference to its description, since its owner may have closed
 * it meanwhile. Returns the descriptor or a negative errno.
 */
static int32_t proc_fd_share(fd_table_t *fds, int procfd, int flags)
{
    fd_entry_t src;
    fd_entry_t *dst;
    int fd = fd_alloc_from(current_proc->fds, 0);

    if (fd < 0) {
        return fd;
    }
    if (fd_peek(fds, procfd, &src, FD_PEEK_REF) != 0) {
        fd_free(current_proc->fds, fd);
        return -ENOENT;
    }
    dst = fd_slot(current_proc->fds, fd);
    dup_fill(dst, &src, (flags & O_CLOEXEC) != 0);
    if (flags & PROC_FD_SHARE_PATH) {
        dst->flags = FD_FLAG_PATH | (dst->flags & FD_FLAG_CLOEXEC);
    }
    fd_ofd_unref(src.ofd);
    fd_install(current_proc->fds, fd);
    return fd;
}

/**
 * A copy of the descriptor the /proc/PID/fd/N link `vp` names, into `*src`,
 * and the process whose table has it, into `*owner`. Another process's
 * descriptor needs a privileged caller or one whose file-system user ID is
 * the process's effective one, which Linux's ptrace access check for a read
 * of /proc compares. The copy is taken under the table's lock and holds what
 * a descriptor in flight holds, a reference to its description and to a
 * socket, since its owner may close it meanwhile without the big lock; the
 * caller gives them back with `fd_peek_drop`. 0, -ENOENT for no such
 * process or descriptor, or -EACCES.
 */
static int32_t proc_fd_entry(const struct vpath *vp, process_t **owner, fd_entry_t *src)
{
    int pid = procfs_resolve_id(vp);
    process_t *p = pid < 0 ? NULL : get_process((uint32_t)pid);

    if (!p || p->state == PROC_UNUSED || !p->fds || vp->procfd < 0) {
        return -ENOENT;
    }
    if (p->fds != current_proc->fds && !cred_privileged() && p->euid != proc_fsuid(current_proc)) {
        return -EACCES;
    }
    if (fd_peek_hold(p->fds, vp->procfd, src) != 0) {
        return -ENOENT;
    }
    *owner = p;
    return 0;
}

int fd_peek_hold(struct fd_table *table, int fd, struct fd_entry *out)
{
    if (fd_peek(table, fd, out, FD_PEEK_REF) != 0) {
        return -EBADF;
    }
    if (out->type == FD_TYPE_SOCKET && out->sock) {
        socket_ref(out->sock);
    }
    return 0;
}

void fd_peek_drop(struct fd_entry *copy)
{
    fd_release_objects(copy);
}

/** Check a descriptor target against NO_XDEV's mount, or allow no rule.
 * The description's mount is stable while nothing sleeps; anonymous
 * objects have no file-system mount and cannot stay on the link's. */
static int proc_fd_xdev(const fd_entry_t *entry, const struct vfs_mount *mnt)
{
    const struct open_file *ofd = entry->ofd;

    return mnt && (!ofd || (ofd->vmnt      ? ofd->vmnt
                            : ofd->dev_mnt ? ofd->dev_mnt
                                           : ofd->synth_mnt) != mnt)
               ? -EXDEV
               : 0;
}

static int open_want(int flags);

/**
 * Open again, as `open_special` opens a device node, the device node `keep`
 * of a mounted file system that a device's description names (`dev_node`):
 * its own mode, owner and group are checked and the device's open runs
 * again. The caller holds the node and its mount (`proc_fd_reopen_device`).
 * Returns the new descriptor, -ENOENT for a mount being unmounted, or the
 * check's or the open's negative errno.
 */
static int32_t proc_fd_reopen_node(const struct vfs_ref *keep, int flags)
{
    struct vfs_mount *mnt = keep->mnt;
    struct vfs_ref ref;
    int32_t rc;
    int fd = fd_alloc(current_proc->fds);

    if (fd < 0) {
        return fd;
    }
    if (vfs_enter(mnt) != 0) {
        fd_free(current_proc->fds, fd);
        return -ENOENT;
    }
    if (mnt->type->ns_serialised) {
        vfs_ns_lock(mnt);
    }
    ref = *keep;
    vfs_ref_hold(&ref);
    if (open_special(&ref, fd, flags, &rc)) {
        return rc;
    }
    vfs_walk_unlock(mnt);
    vfs_ref_release(&ref);
    fd_free(current_proc->fds, fd);
    return -ENXIO;
}

/**
 * Open again the device that the descriptor `src` is open on, as an open(2)
 * of the node it was opened through would, as Linux follows the descriptor's
 * path: the access `flags` ask for is checked against that node's mode, owner
 * and group, whatever access `src` has, and the device's own open runs again,
 * so that the description is a new one and a DRM device gives a new file. A
 * device opened through a device node of tmpfs or ext2 is opened through the
 * node its description names (`proc_fd_reopen_node`), and one opened in /dev
 * through its entry there. The node and its mount are held here, before
 * anything sleeps, and not the description: another thread may close the
 * descriptor meanwhile, and the device's close runs only at the close of a
 * description's last descriptor (`fd_release_objects`), which a reference
 * held here would turn into a release that closes nothing. Returns the new
 * descriptor, -ENXIO when the device is gone, or the check's or the open's
 * negative errno.
 */
static int32_t proc_fd_reopen_device(fd_entry_t *src, int flags)
{
    uint32_t mode = devfs_fd_mode(src), uid, gid;
    const char *name = devfs_name_of((mode & 0170000u) == 0060000u, devfs_rdev(src));
    struct open_file *ofd = src->ofd;
    struct vfs_ref keep;
    int32_t rc;

    if (ofd && ofd->dev_mnt) {
        keep.mnt = ofd->dev_mnt;
        keep.node = ofd->dev_node;
        vfs_ref_hold(&keep);
        vfs_desc_open(keep.mnt);
        rc = proc_fd_reopen_node(&keep, flags);
        vfs_desc_close(keep.mnt, keep.node, NULL);
        return rc;
    }
    if (!name) {
        return -ENXIO;
    }
    devfs_fd_owner(src, &uid, &gid);
    rc = cred_access_bits(uid, gid, mode & 0777u, open_want(flags), 0);
    return rc != 0 ? rc : devfs_open(name, flags);
}

static int32_t proc_fd_open_from(const struct vpath *vp, process_t *p, fd_entry_t *src, int flags);

/**
 * open(2) of /proc/PID/fd/N, which /dev/fd/N, /dev/stdin, /dev/stdout and
 * /dev/stderr are for the caller, following the link to what the descriptor
 * is open on, as Linux does. A file or directory is opened again
 * (`proc_fd_reopen`), with its description held across the sleeps of that
 * open, since another thread may close the descriptor meanwhile, and so is
 * a device of /dev (`proc_fd_reopen_device`). Anything else, a pipe, a
 * socket or a terminal, has the descriptor's own description shared with
 * the new one (`proc_fd_share`), when the descriptor has the access asked
 * for and is no O_DIRECTORY open of what is not a directory; Linux opens a
 * pipe's other end for the other access, and this answers EACCES. Returns
 * the new descriptor, or a negative errno: an error of `proc_fd_entry`,
 * -EACCES, -ENOTDIR, or the open's.
 */
static int32_t proc_fd_open(const struct vpath *vp, int flags)
{
    process_t *p;
    fd_entry_t src;
    int32_t rc;

    rc = proc_fd_entry(vp, &p, &src);
    if (rc != 0) {
        return rc;
    }
    rc = proc_fd_open_from(vp, p, &src, flags);
    fd_peek_drop(&src);
    return rc;
}

/** `proc_fd_open` of the copy `src` of descriptor `vp->procfd` of `p`. */
static int32_t proc_fd_open_from(const struct vpath *vp, process_t *p, fd_entry_t *src, int flags)
{
    unsigned acc = (unsigned)flags & 3u;
    struct open_file *held;
    int32_t rc;

    rc = proc_fd_xdev(src, current_proc->walk_replay_mnt);
    if (rc != 0) {
        return rc;
    }
    if (src->ofd && src->ofd->vmnt && (src->type == FD_TYPE_FILE || src->type == FD_TYPE_DIR)) {
        held = src->ofd;
        fd_ofd_ref(held);
        rc = proc_fd_reopen(held, flags);
        fd_ofd_unref(held);
        return rc;
    }
    if (src->type == FD_TYPE_DEVICE && src->dev) {
        return proc_fd_reopen_device(src, flags);
    }
    if ((src->flags & FD_FLAG_PATH) || (acc != 1 && !(src->flags & FD_FLAG_READ)) ||
        (acc != 0 && !(src->flags & FD_FLAG_WRITE))) {
        return -EACCES;
    }
    if ((flags & O_DIRECTORY) && src->type != FD_TYPE_SYNTH_DIR) {
        return -ENOTDIR;
    }
    return proc_fd_share(p->fds, vp->procfd, flags);
}

int32_t proc_fd_file(const struct vpath *vp, struct vfs_ref *ref, struct open_file **held)
{
    struct vfs_mount *mnt;
    process_t *p;
    fd_entry_t src;
    int32_t rc;

    rc = proc_fd_entry(vp, &p, &src);
    if (rc != 0) {
        return rc;
    }
    if (!src.ofd || !src.ofd->vmnt || (src.type != FD_TYPE_FILE && src.type != FD_TYPE_DIR)) {
        fd_peek_drop(&src);
        return -EACCES;
    }
    *held = src.ofd;
    fd_ofd_ref(*held);
    fd_peek_drop(&src);
    mnt = (*held)->vmnt;
    if (vfs_enter(mnt) != 0) {
        fd_ofd_unref(*held);
        *held = NULL;
        return -ENOENT;
    }
    if (mnt->type->ns_serialised) {
        vfs_ns_lock(mnt);
    }
    ref->mnt = mnt;
    ref->node = (*held)->vnode;
    vfs_ref_hold(ref);
    return 0;
}

/** The access an open with `flags` asks for: CRED_R, CRED_W or both, as
 * its access mode says. */
static int open_want(int flags)
{
    switch (flags & 3) {
    case 1:
        return CRED_W;
    case 2:
        return CRED_R | CRED_W;
    default:
        return CRED_R;
    }
}

/** Open the file of /proc at `vp` that is root's alone, checked for the
 * access `want` as a file of `mode` that root owns: /proc/disk for reading
 * as 0400, whatever `flags` ask, and /proc/acpi/call as 0600. */
static int32_t synth_open_root(const struct vpath *vp, int flags, uint32_t mode, int want)
{
    int32_t rc = cred_access_bits(0, 0, mode, want, 0);

    return rc != 0 ? rc : procfs_open(vp, flags);
}

/**
 * Open with `flags` the node of /dev, /proc or /sys that a path classified
 * as `vp`, and return the descriptor or a negative errno. Nothing is created
 * there, and no name falls through to KRFFS: a name the class does not know
 * is -ENOENT.
 *
 * A device of /dev binds its file operations (`devfs_open`) after the
 * nine-bit check against the mode its entry reports, every entry owned by
 * root: the 0666 devices open for anyone, and the raw boot-partition window,
 * 0600, for root alone. /dev/pts/N is the slave end of pty N, checked
 * against the pty's owner, group and mode, so that one user cannot open
 * another's terminal; `pty_open_slave` takes the descriptor itself and fails
 * on a pair not allocated or locked.
 *
 * A file of /proc or /sys snapshots its text at the open (`procfs_open`),
 * but for oom_score_adj and mem, which reach the process at each read.
 * /proc/disk opens for root alone: it has the drive's serial number, and an
 * image that has the file also has an unprivileged account. A directory gets
 * a synthetic directory descriptor (`synth_open_dir`), /proc/PID/fd/N
 * follows its link (`proc_fd_open`), /proc/PID/fdinfo/N opens for a caller
 * that may read the process, and a namespace link opens its namespace
 * (`procfs_open_ns`).
 */
static int32_t synth_open(const struct vpath *vp, int flags)
{
    if (synth_kind_is_dir(vp->kind)) {
        return synth_open_dir(vp, flags);
    }
    switch (vp->kind) {
    case VNODE_DEV: {
        uint32_t dmode, uid, gid;
        int32_t rc;

        if (devfs_stat(vp->name, &dmode, NULL, NULL) != 0 ||
            devfs_owner(vp->name, &uid, &gid) != 0) {
            return -ENOENT;
        }
        rc = cred_access_bits(uid, gid, dmode & 0777u, open_want(flags), 0);
        return rc != 0 ? rc : devfs_open(vp->name, flags);
    }
    case VNODE_PTS:
        if (flags & O_DIRECTORY) {
            return -ENOTDIR;
        }
        {
            struct pty *pt = pty_get(vp->pts_index);
            if (pt) {
                int32_t rc = cred_access_bits(pt->uid, pt->gid, (uint32_t)(pt->mode & 0777u),
                                              open_want(flags), 0);
                if (rc != 0) {
                    return rc;
                }
            }
        }
        return pty_open_slave(vp->pts_index, flags);
    case VNODE_PROC_FILE:
    case VNODE_PROC_CMDLINE:
    case VNODE_PROC_NET_DEV:
    case VNODE_PROC_UPTIME:
    case VNODE_PROC_STAT_SYS:
    case VNODE_PROC_MEMINFO:
    case VNODE_PROC_CPUINFO:
    case VNODE_PROC_LOADAVG:
    case VNODE_PROC_DISKSTATS:
    case VNODE_PROC_VERSION:
    case VNODE_PROC_PARTITIONS:
    case VNODE_PROC_VMSTAT:
    case VNODE_PROC_SLABINFO:
    case VNODE_PROC_SWAPS:
    case VNODE_PROC_LOCKS:
    case VNODE_PROC_SYS_FILE:
    case VNODE_PROC_MOUNTS:
    case VNODE_PROC_LOOP:
    case VNODE_PROC_AUDIO:
    case VNODE_PROC_USB:
    case VNODE_PROC_FILESYSTEMS:
    case VNODE_PROC_IRQ_FILE:
    case VNODE_PROC_INTERRUPTS:
    case VNODE_PROC_CONSOLE:
    case VNODE_PROC_NET_ROUTE:
    case VNODE_PROC_NET_ARP:
    case VNODE_PROC_NET_TCP:
    case VNODE_PROC_NET_FILE:
    case VNODE_PROC_SYSVIPC:
    case VNODE_PROC_FS_LOCKWAIT:
    case VNODE_SYS_FILE:
        return procfs_open(vp, flags);
    case VNODE_PROC_DISK:
        return synth_open_root(vp, flags, 0400u, CRED_R);
    case VNODE_PROC_ACPI_CALL:
        return synth_open_root(vp, flags, 0600u, open_want(flags));
    case VNODE_DEV_MADE:
        return devtree_is_dir(vp->syskey) == 1 ? synth_open_dir(vp, flags) : -ENOENT;
    case VNODE_ENOTDIR:
        return -ENOTDIR;
    case VNODE_PROC_FD_LINK:
        return proc_fd_open(vp, flags);
    case VNODE_PROC_FDINFO:
        if (procfs_resolve_id(vp) < 0) {
            return -ENOENT;
        }
        return procfs_may_read(procfs_resolve_pid(vp->pid)) ? procfs_open(vp, flags) : -EACCES;
    case VNODE_PROC_NS_LINK:
        if (current_proc->walk_replay_mnt) {
            return -EXDEV;
        }
        return procfs_resolve_id(vp) < 0 ? -ENOENT
                                         : procfs_open_ns(procfs_resolve_id(vp), vp->procfd, flags);
    default:
        return procfs_ino_global(vp->kind) ? procfs_open(vp, flags) : -ENOENT;
    }
}

/** Check an open with `flags` of the synthetic directory at `vp`, of
 * process `pid`, against an ns directory's mode, 0511, and the owner stat
 * reports (`procfs_pid_owner`), as Linux's generic_permission answers a
 * read: 0 for another directory or an O_PATH open, which reads nothing, or
 * for a caller the mode lets read it; -EACCES for a caller other than its
 * owner and root, or -ENOENT for a process that is gone. */
static int32_t proc_ns_dir_read(const struct vpath *vp, int pid, int flags)
{
    uint32_t mode = synth_pid_dir_mode(vp), uid, gid;

    if (vp->kind != VNODE_PROC_NS_DIR || (flags & O_PATH)) {
        return 0;
    }
    if (procfs_pid_owner(pid, mode, &uid, &gid) != 0) {
        return -ENOENT;
    }
    return cred_access_bits(uid, gid, mode, CRED_R, 0);
}

/**
 * The permission check of the `want` bits, CRED_R for an open or CRED_X for
 * a chdir, over the directory of kind `kind` of the process `pid`: 0, or a
 * negative errno. /proc/PID/fd is judged as Linux's `proc_fd_permission`
 * judges it: by its mode, 0500, and its owner (`procfs_pid_owner`), with a
 * thread of the caller's own process always let in. A walk judges the owner
 * the lookup gives it now, and fchdir the one `sd`, the record of a
 * descriptor open on the directory, keeps from its last lookup
 * (`procfs_owner_keep`), as Linux judges the inode's owner, which only a
 * lookup sets (fs/proc/fd.c:331-345); with no record, a process that has
 * ended is -ENOENT. /proc/PID/fdinfo, mode 0555, takes Linux's ptrace check
 * as well (`procfs_may_read`), -EACCES, and -ESRCH once the process has
 * ended, as Linux's `proc_fdinfo_permission` answers. Any other kind is 0.
 */
static int32_t proc_fd_dir_access(int kind, int pid, int want, const struct synth_dir *sd)
{
    uint32_t uid, gid;

    if (kind == VNODE_PROC_FDINFO_DIR) {
        if (procfs_pid_owner(pid, S_IFDIR | 0555, &uid, &gid) != 0) {
            return -ESRCH;
        }
        return procfs_may_read(pid) ? 0 : -EACCES;
    }
    if (kind != VNODE_PROC_FD_DIR) {
        return 0;
    }
    if (procfs_pid_owner(pid, S_IFDIR | 0500, &uid, &gid) != 0) {
        if (!sd) {
            return -ENOENT;
        }
    } else if (procfs_same_group(pid)) {
        return 0;
    }
    if (sd) {
        uid = sd->own.uid;
        gid = sd->own.gid;
    }
    return cred_access_bits(uid, gid, 0500, want, 0);
}

/** Whether a device directory or message directory still exists at open. */
static int synth_dir_present(const struct vpath *vp)
{
    if ((vp->kind == VNODE_SYS_DIR || vp->kind == VNODE_SYS_LINK) && SYSFS_KEYED(vp->sysent)) {
        return sysfs_device_exists(vp->sysent, vp->syskey);
    }
    if (vp->kind == VNODE_DEV_SUBDIR) {
        return devfs_subdir_present(vp->devdir);
    }
    if (vp->kind == VNODE_PROC_IRQ_DIR && vp->pid >= 0) {
        return procfs_irq_exists(vp->pid);
    }
    return 1;
}

/**
 * Open a synthetic directory (/dev, /proc, /proc/<pid> and its fd, fdinfo
 * and ns, a thread's under /proc/<pid>/task, a directory of /proc/sys or of
 * /sys) for reading, and return the descriptor or a negative errno. The
 * descriptor keeps a record of its own (`synth_dir_alloc`), freed at the
 * last close, with the process's pid and, for a thread's directory, the
 * thread's id, as the caller's PID namespace numbers them, which is how its
 * listing, its /proc/PID/fd link and the names relative to it read them, so
 * they stay the thread's; and for an fd, fdinfo or task directory the owner
 * a lookup last gave it (`procfs_owner_keep`).
 *
 * The directory exists, so flags that contradict that fail. O_CREAT with
 * O_EXCL is -EEXIST whatever the access mode, since the usual form of an
 * exclusive create also asks for write access; then write access, or
 * O_CREAT, which asks for a file, is -EISDIR. /proc/PID/fd is read by its
 * mode and owner, and /proc/PID/fdinfo by those who may trace the process
 * (`proc_fd_dir_access`), -EACCES for any other caller. /proc/PID/ns, a
 * thread's among them, is read by its mode and owner (`proc_ns_dir_read`).
 * O_PATH in `flags`, which reads nothing, skips these checks, as Linux's
 * `do_o_path` opens with no permission check. A process, or a device of
 * /sys, that is gone is -ENOENT, as is a directory of /dev that is absent
 * (`devfs_subdir_present`), and a record the heap has no room for is
 * -ENFILE. A directory made under /dev is one the caller found there.
 */
static int32_t synth_open_dir(const struct vpath *vp, int flags)
{
    fd_entry_t *e;
    int fd;
    int pid = 0;

    struct synth_dir *sd;

    if (synth_dir_of_proc(vp->kind)) {
        if (procfs_resolve_id(vp) < 0) {
            return -ENOENT;
        }
        pid = procfs_resolve_pid(vp->pid);
    }
    if ((flags & O_CREAT) && (flags & O_EXCL)) {
        return -EEXIST;
    }
    if ((flags & 3) != 0 || (flags & O_CREAT)) {
        return -EISDIR;
    }
    fd = (flags & O_PATH) ? 0 : proc_fd_dir_access(vp->kind, pid, CRED_R, NULL);
    if (fd != 0) {
        return fd;
    }
    fd = proc_ns_dir_read(vp, pid, flags);
    if (fd != 0) {
        return fd;
    }
    if (!synth_dir_present(vp)) {
        return -ENOENT;
    }
    if (vp->kind == VNODE_SYS_DIR || vp->kind == VNODE_SYS_LINK) {
        sd = synth_dir_alloc(VNODE_SYS_DIR, vp->sysent, vp->syskey);
    } else if (vp->kind == VNODE_PROC_IRQ_DIR) {
        sd = synth_dir_alloc(vp->kind, vp->pid, 0);
    } else if (vp->kind == VNODE_DEV_SUBDIR) {
        sd = synth_dir_alloc(VNODE_DEV_SUBDIR, vp->devdir, 0);
    } else if (vp->kind == VNODE_DEV_MADE) {
        sd = synth_dir_alloc(VNODE_DEV_MADE, vp->syskey, 0);
    } else {
        sd = synth_dir_alloc(
            vp->kind, vp->kind == VNODE_PROC_SYS_DIR ? vp->sysent : (int)pid_seen((uint32_t)pid),
            vp->tid);
    }
    if (!sd) {
        return -ENFILE;
    }
    fd = fd_alloc(current_proc->fds);
    if (fd < 0) {
        kfree(sd);
        return fd;
    }
    procfs_owner_keep(&sd->own, vp);
    e = fd_slot(current_proc->fds, fd);
    e->type = FD_TYPE_SYNTH_DIR;
    e->node = NULL;
    e->sock = NULL;
    e->ops = NULL;
    e->priv = sd;
    e->ofd->offset = 0;
    e->ofd->status_flags |= OFD_OPEN_STATUS(flags); /* none for O_PATH's open */
    e->flags = (flags & O_PATH) ? FD_FLAG_PATH : FD_FLAG_READ;
    if (flags & O_CLOEXEC) {
        e->flags |= FD_FLAG_CLOEXEC;
    }
    fd_install(current_proc->fds, fd);
    return fd;
}

/**
 * Stamp the KRFFS node `node`, new in the directory `parent`, with the
 * creating process's identity: owner and group from the file-system ids,
 * the effective ones unless setfsuid or setfsgid set them apart, as the V7
 * creation rule gives a file to whoever its creator acts as, and the
 * permission bits of `mode` with the file-creation mask applied, the type
 * bits kept. A setgid parent gives its group instead, and its setgid bit to
 * a new directory, as Linux's `inode_init_owner` does.
 */
static void krffs_stamp_new_node(krffs_node_t *node, const krffs_node_t *parent, uint32_t mode)
{
    uint32_t perm = mode & 07777u & ~current_proc->fs->umask;

    node->uid = proc_fsuid(current_proc);
    node->gid = proc_fsgid(current_proc);
    if (parent->mode & 02000u) {
        node->gid = parent->gid;
        if ((node->mode & 0170000u) == 0040000u) {
            perm |= 02000u;
        }
    }
    node->mode = (uint16_t)((node->mode & ~07777u) | perm);
}

static int synth_is_link(const struct vpath *vp);

/**
 * Whether `name` is the literal `lit`, byte for byte.
 */
static int name_is(const char *name, const char *lit)
{
    while (*name != '\0' && *name == *lit) {
        name++;
        lit++;
    }
    return *name == *lit;
}

/**
 * The framebuffer descriptor, for an open of the name "fb" with the open's
 * `flags`, or a negative errno: -EBUSY unless the caller owns the
 * framebuffer or descends from its owner, and -ENOMEM. One process at a
 * time owns it, by its thread group id, which
 * the descriptor's operations (display/fbdev.c), the console read and the
 * last close compare too, so any of its threads may use it. The open takes
 * the display at mode 13h for a new owner (`fb_take`), unblanking it first;
 * a second open by the same owner keeps its mode. The console's
 * backlight would stay off under the picture. O_CLOEXEC and the status
 * flags (`OFD_OPEN_STATUS`) are kept; the access mode is not, since the
 * descriptor reads and writes.
 */
static int32_t open_framebuffer(int flags)
{
    fd_entry_t *entry;
    int fd;
    int32_t rc;

    fd = fd_alloc(current_proc->fds);
    if (fd < 0) {
        return fd;
    }
    entry = fd_slot(current_proc->fds, fd);
    rc = fb_take(entry);
    if (rc != 0) {
        fd_free(current_proc->fds, fd);
        return rc;
    }

    entry = fd_slot(current_proc->fds, fd);
    entry->type = FD_TYPE_FRAMEBUFFER;
    entry->node = NULL;
    entry->ops = &fb_file_ops;
    entry->ofd->offset = 0;
    entry->ofd->status_flags = OFD_OPEN_STATUS(flags);
    entry->flags = FD_FLAG_READ | FD_FLAG_WRITE | ((flags & O_CLOEXEC) ? FD_FLAG_CLOEXEC : 0u);

    fd_install(current_proc->fds, fd);
    return fd;
}

/**
 * The mouse descriptor, read-only, for an open of the name "mouse" with the
 * open's `flags`, with the packets queued before the open dropped; -ENODEV
 * when no mouse of any kind was found. O_CLOEXEC and the status flags
 * (`OFD_OPEN_STATUS`) are kept.
 */
static int32_t open_mouse(int flags)
{
    fd_entry_t *entry;
    int fd;

    if (!input_mouse_present()) {
        return -ENODEV;
    }

    fd = fd_alloc(current_proc->fds);
    if (fd < 0) {
        return fd;
    }

    entry = fd_slot(current_proc->fds, fd);
    entry->type = FD_TYPE_MOUSE;
    entry->node = NULL;
    entry->ofd->offset = 0;
    entry->ofd->status_flags = OFD_OPEN_STATUS(flags);
    entry->flags = FD_FLAG_READ | ((flags & O_CLOEXEC) ? FD_FLAG_CLOEXEC : 0u);

    mouse_clear_packets();

    fd_install(current_proc->fds, fd);
    return fd;
}

/**
 * Whether `name` is the legacy device name `lit`: 1 for the name alone, 2
 * for the name followed by the last "." or ".." a path that names a
 * directory by its end keeps (`path_put_dot`), 0 for any other name.
 */
static int device_name(const char *name, const char *lit)
{
    while (*lit != '\0' && *name == *lit) {
        name++;
        lit++;
    }
    if (*lit != '\0') {
        return 0;
    }
    if (*name == '\0') {
        return 1;
    }
    return (name_is(name, "/.") || name_is(name, "/..")) ? 2 : 0;
}

/**
 * The open of a legacy device name, "fb" or "mouse", which `do_open` makes
 * before any walk: 1 with the descriptor or the errno in `*result` when
 * `name` is one of the two, 0 when it is not. Neither device is a
 * directory, so the name with a last "." or "..", and O_DIRECTORY, which a
 * trailing slash sets (`open_named`), are -ENOTDIR, before the display or
 * the mouse is touched. O_CLOEXEC and the status flags are kept, and the
 * other flags ignored.
 */
static int open_device_name(const char *name, int flags, int *result)
{
    int fb = device_name(name, "fb");
    int mouse = device_name(name, "mouse");

    if (fb == 0 && mouse == 0) {
        return 0;
    }
    if (fb == 2 || mouse == 2 || (flags & O_DIRECTORY)) {
        *result = -ENOTDIR;
    } else {
        *result = fb ? open_framebuffer(flags) : open_mouse(flags);
    }
    return 1;
}

/**
 * The target of a symbolic link at `name` whose own target is missing, for
 * O_CREAT to create: as on Linux, the target is what is created. Returns 1
 * with the target's path in `through`, the link's directory in front of a
 * relative one; 0 when `name` is not a link; or a negative errno: ELOOP once
 * `followed` passes the resolver's bound of forty links, ENOENT for an empty
 * target, which names nothing, as the resolver answers, ENOMEM, or
 * ENAMETOOLONG. `name` may be in `through`, from the link before.
 */
static int open_link_target(const char *name, char *through, uint32_t through_size, int *followed)
{
    struct vfs_ref linkref;
    struct vfs_stat lst;
    char *target;
    int tlen;
    int is_link;

    if (vfs_resolve(0, name, VFS_WALK_NOFOLLOW, &linkref, 0, 0) != 0) {
        return 0;
    }
    is_link = !vfs_synth_vpath(linkref.mnt, linkref.node) && linkref.mnt->type->ops->readlink &&
              vfs_getattr(linkref.mnt, linkref.node, &lst) == 0 &&
              (lst.mode & 0170000u) == 0120000u;
    if (!is_link) {
        vfs_ref_release(&linkref);
        return 0;
    }
    target = path_buf_get();
    if (!target) {
        vfs_ref_release(&linkref);
        return -ENOMEM;
    }
    tlen = linkref.mnt->type->ops->readlink(linkref.mnt, linkref.node, target, VFS_PATH_MAX);
    vfs_ref_release(&linkref);
    if (tlen == 0) {
        tlen = -ENOENT;
    }
    if (tlen >= 0 && ++*followed > VFS_LINK_MAX) {
        tlen = -ELOOP;
    }
    if (tlen >= 0 && target[0] != '/') {
        uint32_t dlen = (uint32_t)k_strlen(name);

        while (dlen > 0 && name[dlen - 1] != '/') {
            dlen--;
        }
        if (dlen + (uint32_t)tlen + 1 > through_size) {
            tlen = -ENAMETOOLONG;
        } else {
            k_memmove(through, name, dlen);
            k_memcpy(through + dlen, target, (uint32_t)tlen + 1);
        }
    } else if (tlen >= 0) {
        k_memcpy(through, target + 1, (uint32_t)tlen);
    }
    path_buf_put(target);
    return tlen < 0 ? tlen : 1;
}

/**
 * Make `leaf` in the held directory `parent`, whose namespace lock the
 * caller holds: a regular file for a `mode` with no type bits or S_IFREG's,
 * and a FIFO, a socket or a device node with the number `rdev` for theirs.
 * The permission bits are masked by the umask and the owner is the caller;
 * the KRFFS root takes both from the stamp after the node is made. Returns
 * 0 with the new node held in `*made`, or the type's errno.
 */
static int node_make(const struct vfs_ref *parent, const char *leaf, uint32_t mode, uint32_t rdev,
                     void **made)
{
    uint32_t type = mode & S_IFMT;
    uint32_t perm =
        mnt_is_root(parent->mnt) ? (mode & 07777u) : (mode & 07777u & ~current_proc->fs->umask);
    int rc;

    if (type == 0 || type == S_IFREG) {
        rc = vfs_create(parent->mnt, parent->node, leaf, perm, made);
    } else {
        rc = vfs_mknod(parent->mnt, parent->node, leaf, type | perm, rdev, made);
    }
    if (rc == 0 && mnt_is_root(parent->mnt)) {
        krffs_stamp_new_node((krffs_node_t *)*made, (krffs_node_t *)parent->node, mode);
    }
    return rc;
}

/* What `dir_recheck` checks again under a directory's lock. */
#define RECHECK_WRITE 1 /* write and search permission on the directory */
#define RECHECK_MOUNT 2 /* a mount on the name is -EBUSY */
#define RECHECK_NEW   4 /* a mount on the name is -EEXIST, for a new name */
#define RECHECK_ROOT  8 /* a process root or a bind's root at the name is -EBUSY */

/**
 * Check again, under the lock of the directory `parent` (`vfs_node_lock`),
 * what a caller checked of it and of its name `leaf` before it took the
 * lock, the checks `what` names, on a type that sets `file_locked`, whose
 * walk held nothing, so that a chmod, a mount or a chroot that ran while
 * the caller slept is seen. 0 on any other type, whose walk or namespace
 * lock kept them, or the first refusal.
 */
static int32_t dir_recheck(const struct vfs_ref *parent, const char *leaf, int what)
{
    int32_t rc = 0;

    if (!vfs_file_locked(parent->mnt)) {
        return 0;
    }
    if (what & RECHECK_WRITE) {
        rc = cred_access_ref(parent, CRED_W | CRED_X, 0);
    }
    if (rc == 0 && (what & (RECHECK_MOUNT | RECHECK_NEW)) &&
        vfs_leaf_mount(parent->mnt, parent->node, leaf)) {
        rc = (what & RECHECK_NEW) ? -EEXIST : -EBUSY;
    }
    if (rc == 0 && (what & RECHECK_ROOT) && vfs_leaf_root(parent->mnt, parent->node, leaf)) {
        rc = -EBUSY;
    }
    return rc;
}

/**
 * Take the lock of the directory `parent` for a change to its name `leaf`
 * (`vfs_node_lock`) and check again under it what `what` names
 * (`dir_recheck`). Returns 0 or the refusal; the lock is held either way,
 * and the caller gives it back with `vfs_node_unlock`.
 */
static int32_t dir_lock(const struct vfs_ref *parent, const char *leaf, int what)
{
    vfs_node_lock(parent);
    return dir_recheck(parent, leaf, what);
}

/**
 * The missing-file half of O_CREAT: hold the parent of `name`, check its
 * write bit, and make the node under the namespace lock with `node_make`,
 * of the kind the type bits of `mode` give, which an open passes none of.
 * Returns 0 with `ref` on the new node, held by its creation, and the parent
 * node's hold given back; 1 when another creator held the lock first and
 * O_EXCL is clear, so the caller opens what it made; or a negative errno:
 * -EROFS under /dev or /proc or on a read-only mount, the permission's, or
 * the type's.
 *
 * The create runs under the parent's lock on every type (`vfs_node_lock`):
 * the mount's namespace lock, or the directory's own on a type that sets
 * `file_locked`. The walk takes the namespace lock only on a type that sets
 * `ns_serialised`, and there success keeps it through the registration of
 * the open handle; on another type the lock is taken around `node_make`
 * alone and given back at once, and on a type that sets `file_locked` the
 * write permission is checked again under it (`dir_lock`).
 */
static int open_create(const char *name, int flags, uint32_t mode, struct vfs_ref *ref,
                       struct vfs_name **entry)
{
    struct vfs_ref parent;
    char leafname[VFS_NAME_MAX + 1];
    void *made = NULL;
    int held;
    int rc;

    rc = vfs_resolve(0, name, VFS_WALK_PARENT | VFS_WALK_LOCK, &parent, leafname, sizeof(leafname));
    if (rc != 0) {
        return rc;
    }
    held = parent.mnt->type->ns_serialised;
    if (vfs_synth_vpath(parent.mnt, parent.node)) {
        vfs_walk_unlock(parent.mnt);
        vfs_ref_release(&parent);
        return -EROFS; /* nothing is created under /dev or /proc */
    }
    if (mnt_is_root(parent.mnt)) {
        rc = (int)cred_access_node((krffs_node_t *)parent.node, CRED_W, 0);
    } else if (parent.mnt->flags & VFS_MS_RDONLY) {
        rc = -EROFS;
    } else {
        rc = (int)cred_access_ref(&parent, CRED_W, 0);
    }
    if (rc != 0) {
        vfs_walk_unlock(parent.mnt);
        vfs_ref_release(&parent);
        return rc;
    }
    if (!held) {
        rc = dir_lock(&parent, leafname, RECHECK_WRITE);
    }
    if (rc == 0) {
        rc = node_make(&parent, leafname, mode, 0, &made);
    }
    if (rc == 0 && entry && !mnt_is_root(parent.mnt)) {
        rc = vfs_name_capture(&parent, leafname, entry);
        if (rc != 0) {
            struct vfs_ref child;

            child.mnt = parent.mnt;
            child.node = made;
            vfs_ref_put(&child);
            rc = -ENOMEM;
        }
    }
    if (!held) {
        vfs_node_unlock(&parent);
    }
    if (rc == -EEXIST && !(flags & O_EXCL)) {
        vfs_walk_unlock(parent.mnt);
        vfs_ref_release(&parent);
        return 1;
    }
    if (rc != 0) {
        vfs_walk_unlock(parent.mnt);
        vfs_ref_release(&parent);
        return rc;
    }
    ref->mnt = parent.mnt;
    ref->node = made;
    if (parent.mnt->type->ops->put) {
        parent.mnt->type->ops->put(parent.mnt, parent.node);
    }
    return 0;
}

/**
 * The FIFO half of `open_special`: the descriptor `fd` the open reserved, whose
 * description holds the file and its mount, as a regular file's does, made an
 * end of the file's pipe. The walk's lock goes before `fifo_attach`, which may
 * wait for the other end, with the slot still a reservation, which no lookup
 * finds until it is published. Returns the descriptor or a negative errno,
 * with `ref` released and the reservation installed or freed either way.
 */
static int32_t open_fifo(struct vfs_ref *ref, int fd, uint64_t ino, int flags)
{
    struct vfs_mount *mnt = ref->mnt;
    fd_entry_t *e = fd_slot(current_proc->fds, fd);
    int32_t rc;

    rc = vfs_open_node(mnt, ref->node, flags, &e->ofd->fhandle);
    if (rc != 0) {
        vfs_walk_unlock(mnt);
        vfs_ref_release(ref);
        fd_free(current_proc->fds, fd);
        return rc;
    }
    e->ofd->vmnt = mnt;
    vfs_wb_err_sample(mnt, &e->ofd->wb_seen, &e->ofd->sb_seen);
    e->ofd->vnode = ref->node;
    e->ofd->ino = ino;
    e->ofd->ino_seen = vfs_ino_err_sample(mnt, ino);
    vfs_desc_open(mnt);
    vfs_walk_unlock(mnt);
    vfs_leave(mnt);
    rc = fifo_attach(e, vfs_mount_base(mnt), ino, flags);
    if (rc != 0) {
        fd_release_orphan(e);
        fd_free(current_proc->fds, fd);
        return rc;
    }
    fd_install(current_proc->fds, fd);
    return fd;
}

/**
 * The device half of `open_special`: the /dev device a device node's number
 * and kind name, opened as an open of its /dev path opens it, without the
 * permission check the node's own mode has already made. A pseudo-terminal
 * slave is major 136 and its index. A number no device present has is
 * ENXIO.
 */
static int32_t open_device(int block, uint32_t rdev, int flags)
{
    uint32_t major = (rdev >> 8) & 0xFFFu;
    uint32_t minor = (rdev & 0xFFu) | ((rdev >> 12) & 0xFFF00u);
    const char *name;

    if (!block && major == 136) {
        return pty_get((int)minor) ? pty_open_slave((int)minor, flags) : -ENXIO;
    }
    name = devfs_name_of(block, rdev);
    return name ? devfs_open(name, flags) : -ENXIO;
}

/**
 * The device half of `open_special` for the device node at `ref`, whose
 * checks have passed: the device opened (`open_device`) into the lowest free
 * descriptor, as `fd`, the open's reservation, is freed first, with the node
 * kept by the new description (`dev_node`). Until then the node is kept by a
 * description of its own, since the device's open may sleep. Only tmpfs and
 * ext2 make device nodes, and the hold of either keeps a node with no name
 * until its last put. Returns the descriptor or a negative errno, with `ref`
 * and the walk's lock released either way.
 */
static int32_t open_device_node(struct vfs_ref *ref, int fd, int block, uint32_t rdev, int flags)
{
    struct vfs_mount *mnt = ref->mnt;
    struct open_file *keep = fd_ofd_new(), *to;
    int32_t rc;

    if (!keep) {
        vfs_walk_unlock(mnt);
        vfs_ref_release(ref);
        fd_free(current_proc->fds, fd);
        return -ENOMEM;
    }
    keep->dev_mnt = mnt;
    keep->dev_node = ref->node;
    vfs_desc_open(mnt);
    vfs_walk_unlock(mnt);
    vfs_leave(mnt);
    fd_free(current_proc->fds, fd);
    rc = open_device(block, rdev, flags);
    to = rc >= 0 ? fd_slot(current_proc->fds, rc)->ofd : NULL;
    if (to && !to->dev_mnt) {
        to->dev_mnt = keep->dev_mnt;
        to->dev_node = keep->dev_node;
        keep->dev_mnt = NULL;
        keep->dev_node = NULL;
    }
    fd_ofd_unref(keep);
    return rc;
}

/**
 * Open the FIFO, the socket file or the device node at `ref`, which name no
 * bytes of their own: 1 with the descriptor or the errno in `*result`, `ref`
 * and the walk's lock released, and the descriptor `fd` the open reserved
 * taken, by a FIFO, or freed, since a device's open takes a descriptor of
 * its own; 0, with all three as they were, for any other node, which the
 * ordinary open takes, and for a node of a mounted type whose attributes
 * cannot be read, so that the ordinary open reports the error.
 *
 * The checks are open's, in its order: O_CREAT|O_EXCL is EEXIST,
 * O_DIRECTORY is ENOTDIR, then the permission the access mode asks for; a
 * device node on a mount with MS_NODEV is EACCES, as on Linux. O_TRUNC is
 * ignored, and a read-only mount does not refuse an open for writing, since
 * nothing is written to the file. A socket file is ENXIO, as it is on Linux,
 * whose sockets are reached by connect(2).
 *
 * A FIFO's pipe is found by the file's mount and a number that names the
 * file alone for as long as an end is open: a mounted type's inode number,
 * and on the KRFFS root the node's address, since the root numbers a file
 * by its directory and its name, which a rename changes and two names can
 * share. The mount is the base (`vfs_mount_base`), and a bind of the root
 * keys a FIFO by its node as well, so every alias of the file reaches one
 * pipe. The node stays where it is, a FIFO having no bytes to move, and
 * keeps its place, nameless after an unlink, while an end holds it
 * (`fd_node_holders`).
 */
static int open_special(struct vfs_ref *ref, int fd, int flags, int32_t *result)
{
    struct vfs_mount *mnt = ref->mnt;
    unsigned acc = (unsigned)flags & 3u;
    int want = acc == 0 ? CRED_R : (acc == 1 ? CRED_W : (CRED_R | CRED_W));
    uint32_t type, rdev = 0;
    uint64_t ino;
    int32_t rc;

    if (mnt_is_root(vfs_mount_base(mnt))) {
        krffs_node_t *node = ref->node;

        if (node == krffs_fs->node) {
            return 0;
        }
        type = krffs_mode_type(node->mode);
        ino = (uint64_t)(uintptr_t)node;
    } else {
        struct vfs_stat st;

        if (vfs_getattr(mnt, ref->node, &st) != 0) {
            return 0;
        }
        type = st.mode & S_IFMT;
        rdev = st.rdev;
        ino = st.ino;
    }
    if (type != S_IFIFO && type != S_IFSOCK && type != S_IFCHR && type != S_IFBLK) {
        return 0;
    }
    if ((flags & O_CREAT) && (flags & O_EXCL)) {
        rc = -EEXIST;
    } else if (flags & O_DIRECTORY) {
        rc = -ENOTDIR;
    } else if (mnt_is_root(mnt)) {
        rc = cred_access_node((krffs_node_t *)ref->node, want, 0);
    } else {
        rc = cred_access_ref(ref, want, 0);
    }
    if (rc == 0 && (type == S_IFCHR || type == S_IFBLK) && (mnt->flags & VFS_MS_NODEV)) {
        rc = -EACCES;
    }
    if (rc == 0 && type == S_IFIFO) {
        *result = open_fifo(ref, fd, ino, flags);
        return 1;
    }
    if (rc == 0 && type != S_IFSOCK) {
        *result = open_device_node(ref, fd, type == S_IFBLK, rdev, flags);
        return 1;
    }
    vfs_walk_unlock(mnt);
    vfs_ref_release(ref);
    fd_free(current_proc->fds, fd);
    *result = rc == 0 ? -ENXIO : rc;
    return 1;
}

/**
 * The checks on an existing node of a mounted type before an open of it with
 * `flags`, from its attributes, in the order open(2) keeps: EEXIST for
 * O_CREAT|O_EXCL; ELOOP for a symbolic link itself, which the walk stops at
 * under O_NOFOLLOW; EISDIR for a directory opened for writing, with O_TRUNC
 * or with O_CREAT, which asks for a file, as on Linux; ENOTDIR for
 * O_DIRECTORY on anything else. Then come EROFS for a write-side request on a
 * read-only mount; EPERM for an append-only file opened for writing without
 * O_APPEND or with O_TRUNC, as Linux's may_open answers; the permission the
 * access mode asks for, O_TRUNC asking for write; and EBUSY for O_TRUNC of a
 * file that backs a loop device. Returns 0, or a negative errno with the
 * reference released. The truncation is `open_install`'s, through the open
 * handle.
 */
static int open_mounted_node(struct vfs_ref *ref, int flags)
{
    void *node = ref->node;
    int rc;
    struct vfs_stat vst;
    unsigned acc = (unsigned)flags & 3u;
    int want = acc == 0 ? CRED_R : (acc == 1 ? CRED_W : (CRED_R | CRED_W));

    rc = vfs_getattr(ref->mnt, node, &vst);
    if (rc != 0) {
        vfs_ref_release(ref);
        return rc;
    }
    if ((flags & O_CREAT) && (flags & O_EXCL)) {
        vfs_ref_release(ref);
        return -EEXIST;
    }
    if ((vst.mode & 0170000u) == 0120000u) {
        vfs_ref_release(ref);
        return -ELOOP;
    }
    if ((vst.mode & 0170000u) == 0040000u && (acc != 0 || (flags & (O_TRUNC | O_CREAT)))) {
        vfs_ref_release(ref);
        return -EISDIR;
    }
    if ((vst.mode & 0170000u) != 0040000u && (flags & O_DIRECTORY)) {
        vfs_ref_release(ref);
        return -ENOTDIR;
    }
    if ((acc != 0 || (flags & O_TRUNC)) && (ref->mnt->flags & VFS_MS_RDONLY)) {
        vfs_ref_release(ref);
        return -EROFS;
    }
    if (vst.flags_known && (vst.flags & VFS_FL_APPEND) &&
        ((acc != 0 && !(flags & O_APPEND)) || (flags & O_TRUNC))) {
        vfs_ref_release(ref);
        return -EPERM;
    }
    if (flags & O_TRUNC) {
        want |= CRED_W;
    }
    rc = (int)cred_access_ref(ref, want, 0);
    if (rc != 0) {
        vfs_ref_release(ref);
        return rc;
    }
    if ((flags & O_TRUNC) && loop_backs(ref->mnt, node)) {
        vfs_ref_release(ref);
        return -EBUSY;
    }
    return 0;
}

/**
 * The same checks on a node of the KRFFS root: EEXIST for O_CREAT|O_EXCL;
 * EISDIR for the root or a directory opened for writing, with O_TRUNC or
 * with O_CREAT, which asks for a file; ENOTDIR for O_DIRECTORY on anything
 * else; then the permission the access mode asks for, O_TRUNC asking for
 * write. The truncation is done by `open_install` after it reserves write
 * access, so a running program gives -ETXTBSY. Returns 0, or a negative
 * errno with the reference released.
 */
static int open_root_node(struct vfs_ref *ref, int flags)
{
    krffs_node_t *node = ref->node;
    int is_dir = (node == krffs_fs->node) || KRFFS_IS_DIR(node->mode);
    unsigned acc = (unsigned)flags & 3u;
    int want;
    int32_t crc;

    if ((flags & O_CREAT) && (flags & O_EXCL)) {
        vfs_ref_release(ref);
        return -EEXIST;
    }
    if (is_dir && (acc != 0 || (flags & (O_TRUNC | O_CREAT)))) {
        vfs_ref_release(ref);
        return -EISDIR;
    }
    if (!is_dir && (flags & O_DIRECTORY)) {
        vfs_ref_release(ref);
        return -ENOTDIR;
    }
    if (acc == 0) {
        want = CRED_R;
    } else if (acc == 1) {
        want = CRED_W;
    } else {
        want = CRED_R | CRED_W;
    }
    if (flags & O_TRUNC) {
        want |= CRED_W;
    }
    crc = cred_access_node(node, want, 0);
    if (crc != 0) {
        vfs_ref_release(ref);
        return crc;
    }
    return 0;
}

/**
 * O_TRUNC of the description `ofd`, open on the mount `mnt`: the file cut to
 * nothing under its lock, the set-id bits cleared for a caller without
 * privilege and the pages of its shared mappings let go. 0 or the file
 * system's error. May sleep, and takes `ofd` because the descriptor table
 * may move meanwhile.
 */
static int open_truncate(struct open_file *ofd, struct vfs_mount *mnt)
{
    struct vfs_ref file;
    int own = file_lock(ofd, &file);
    int rc;

    rc = vfs_truncate(mnt, ofd->fhandle, 0);
    if (rc == 0 && !cred_fs_privileged()) {
        if (mnt_is_root(mnt)) {
            cred_kill_setid(ofd->vnode);
        } else {
            vfs_kill_setid(mnt, ofd->vnode);
        }
    }
    if (rc == 0 && own) {
        filemap_file_truncated(mnt, ofd->ino, 0);
    }
    file_unlock(&file, own);
    return rc;
}

/**
 * The descriptor for an opened node, in the slot `fd` the open reserved,
 * with `name`, the path without its leading slash, recorded for a node of a
 * mounted type (`ofd_record_path`): a directory opens as a FD_TYPE_DIR (its
 * node is the directory whose children getdents lists); a regular file as
 * FD_TYPE_FILE. A path that resolved to the root (e.g. "x/..") is the root
 * directory, stored with a NULL node like a literal "/" open. A node of
 * another type keeps `node` NULL: the description's own fields name it, and
 * the KRFFS-only paths (the holder rule, the set-id clear) have nothing to
 * do with it. Returns the fd or a negative errno, with the reference given
 * to the description or, on an error, released and the reservation freed.
 *
 * The description takes the resolution's hold and one reference on the
 * mount, which its last close gives back (`vfs_desc_close`). It is
 * registered before an O_TRUNC of a mounted type's file, so that a
 * relocation while the truncation waits updates its node; a failed
 * truncation frees the description and its open handle, and one that
 * succeeds clears the set-id bits for a caller without privilege. The
 * access mode is the low two bits of `flags`, read apart from the bits above
 * them. O_APPEND, O_NONBLOCK, O_SYNC and O_DSYNC are status flags of the
 * description, which dup and fork share, and O_CLOEXEC, which opendir and
 * fopen's "e" mode rely on, marks the descriptor. A file of debugfs gets
 * the operations its registrant gave it (`debugfs_bind`), which read, write
 * and ioctl call in place of the file system's, and whose open may refuse
 * or sleep.
 *
 * The entry is published last (`fd_install`): until then the slot is a
 * reservation, which no other thread's lookup, close or dup2 can take while
 * the file system's open, the truncation or a debugfs file's open sleeps
 * with the description in use.
 */
static int32_t open_install(struct vfs_ref *ref, int fd, const char *name, int flags)
{
    fd_entry_t *entry = fd_slot(current_proc->fds, fd);
    krffs_node_t *node = ref->node;
    fd_type_t type;
    int rc;

    if (!mnt_is_root(ref->mnt)) {
        struct vfs_stat vst;

        rc = vfs_getattr(ref->mnt, node, &vst);
        if (rc != 0) {
            fd_free(current_proc->fds, fd);
            vfs_ref_release(ref);
            return rc;
        }
        type = (vst.mode & 0170000u) == 0040000u ? FD_TYPE_DIR : FD_TYPE_FILE;
        entry->ofd->ino = vst.ino;
        ofd_record_path(entry->ofd, name);
    } else if (node == krffs_fs->node) {
        type = FD_TYPE_DIR;
        entry->ofd->ino = 1;
    } else {
        type = KRFFS_IS_DIR(node->mode) ? FD_TYPE_DIR : FD_TYPE_FILE;
        entry->ofd->ino = krffs_node_ino(node);
    }
    entry->ofd->offset = 0;

    rc = 0;
    if ((flags & 3) != 0 || (flags & O_TRUNC)) {
        rc = fd_ofd_access(entry->ofd, ref, 1);
    }
    if (rc == 0) {
        rc = vfs_open_node(ref->mnt, node, flags, &entry->ofd->fhandle);
    }
    if (rc != 0) {
        fd_free(current_proc->fds, fd);
        vfs_ref_release(ref);
        return rc;
    }
    entry->ofd->vmnt = ref->mnt;
    vfs_wb_err_sample(ref->mnt, &entry->ofd->wb_seen, &entry->ofd->sb_seen);
    entry->ofd->ino_seen = vfs_ino_err_sample(ref->mnt, entry->ofd->ino);
    entry->ofd->vnode = node;
    vfs_desc_open(ref->mnt);
    vfs_leave(ref->mnt);
    if (flags & O_TRUNC) {
        rc = open_truncate(entry->ofd, ref->mnt);
        if (rc != 0) {
            fd_free(current_proc->fds, fd);
            return rc;
        }
    }

    if ((flags & 3) == 0) {
        fd_ofd_access_drop(entry->ofd);
    }
    if ((flags & 3) == 1) {
        entry->flags = FD_FLAG_WRITE;
    } else if ((flags & 3) == 2) {
        entry->flags = FD_FLAG_READ | FD_FLAG_WRITE;
    } else {
        entry->flags = FD_FLAG_READ;
    }
    if (entry->ofd) {
        entry->ofd->status_flags |= OFD_OPEN_STATUS(flags);
    }
    if (flags & O_CLOEXEC) {
        entry->flags |= FD_FLAG_CLOEXEC;
    }
    entry->node = mnt_is_root(entry->ofd->vmnt) && entry->ofd->vnode != krffs_fs->node
                      ? (krffs_node_t *)entry->ofd->vnode
                      : NULL;
    if (type == FD_TYPE_FILE && vfs_mount_base(entry->ofd->vmnt)->type == &vfs_debugfs_type) {
        rc = debugfs_bind(entry->ofd->vnode, entry, flags);
        if (rc != 0) {
            fd_free(current_proc->fds, fd);
            return rc;
        }
    }
    entry->type = type;
    fd_install(current_proc->fds, fd);
    return fd;
}

/**
 * The open of the synthetic node `vp` a resolution arrived at, for
 * `do_open`. The /proc links, /proc/self and the descriptor links, are links
 * too, and give a mounted type's two answers: ELOOP under O_NOFOLLOW and
 * EEXIST under O_CREAT|O_EXCL. Anything else opens as its kind opens.
 */
static int32_t open_synth_node(const struct vpath *vp, int flags)
{
    if (synth_is_link(vp) && (flags & O_NOFOLLOW)) {
        return -ELOOP;
    }
    if (synth_is_link(vp) && (flags & O_CREAT) && (flags & O_EXCL)) {
        return -EEXIST;
    }
    return synth_open(vp, flags);
}

/**
 * Whether a request with the open flags `flags` of the place of /proc that
 * `ref` holds is refused with EROFS: a write-side request through a
 * read-only mount of /proc, such as the bind bubblewrap makes of
 * /proc/sys, as open(2) and access(2) refuse it on any other read-only
 * mount (`open_mounted_node`, `do_access`). A directory is refused only
 * with `dirs`, for access(2), since open(2) answers EISDIR for it
 * (`synth_open_dir`). /proc/PID/fd/N opens the file it refers to, whose own
 * mount decides. /sys is mounted read-only though its attributes take
 * writes, and keeps them, as do the devices of /dev.
 */
static int proc_read_only(const struct vfs_ref *ref, int flags, int dirs)
{
    const struct vpath *vp = vfs_synth_vpath(ref->mnt, ref->node);
    struct vfs_stat st;

    return ((flags & 3) != 0 || (flags & O_TRUNC)) && (ref->mnt->flags & VFS_MS_RDONLY) &&
           vfs_mount_base(ref->mnt)->type == &vfs_procfs_type && vp &&
           vp->kind != VNODE_PROC_FD_LINK && vfs_getattr(ref->mnt, ref->node, &st) == 0 &&
           (dirs || (st.mode & 0170000u) != 0040000u);
}

/**
 * Whether an open with O_CREAT and without O_EXCL may open the existing file
 * `ref`, found in the directory of attributes `dir`, as Linux's
 * may_create_in_sticky (fs/namei.c:1248-1291): 0, or -EACCES for a file in
 * a sticky directory that others may write, owned by neither the caller
 * nor the directory's owner. A regular file is judged so only under
 * fs.protected_regular and a FIFO under fs.protected_fifos, and at level 2
 * of its sysctl one in a directory its group may write is refused too;
 * any other kind is judged whatever the sysctls say, as Linux judges it.
 * Root is refused as well: the rule keeps a privileged program from writing
 * to a file another user made in its place. The caller holds `ref`.
 */
static int32_t may_create_in_sticky(const struct vfs_ref *ref, const struct vfs_stat *dir)
{
    struct vfs_stat st;
    uint32_t type;
    int level = 1;
    int32_t rc;

    if (!(dir->mode & 01000u)) {
        return 0;
    }
    rc = vfs_getattr(ref->mnt, ref->node, &st);
    if (rc != 0) {
        return rc;
    }
    type = st.mode & S_IFMT;
    if (type == S_IFREG) {
        level = vfs_protected_regular;
    } else if (type == S_IFIFO) {
        level = vfs_protected_fifos;
    }
    if (level == 0 || st.uid == dir->uid || st.uid == proc_fsuid(current_proc)) {
        return 0;
    }
    if ((dir->mode & 0002u) || (level >= 2 && (dir->mode & 0020u))) {
        return -EACCES;
    }
    return 0;
}

/**
 * Open core shared by open and openat.
 *
 * `name` is a kernel-space path with any leading '/' already stripped; `flags`
 * are the Linux open flags and `mode` the permission bits to stamp on a file
 * created by O_CREAT (ignored otherwise). Opens a legacy device ("fb" or
 * "mouse", `open_device_name`) or resolves a path on any mounted type and
 * installs a descriptor. Returns the fd or -errno. Opening "/" opens the
 * root directory for getdents64. A symbolic link at the end is followed,
 * and one whose target is missing is what O_CREAT creates, as on Linux, the
 * target's path built in a path page of its own; O_NOFOLLOW answers ELOOP
 * for the link, and O_CREAT|O_EXCL EEXIST. On a type that sets
 * `ns_serialised`, resolution through open-handle registration holds the
 * final mount's namespace lock, which is what keeps a writer from moving the
 * file between the lookup and the registration. O_PATH goes to `open_path`,
 * which names the file and opens nothing.
 *
 * A file the open creates is empty, so O_TRUNC does nothing to it, and
 * its description is marked OFD_WATCH_MADE, so that inotify reports no
 * truncation of it (`open_done`). An existing file opened with O_CREAT and
 * without O_EXCL meets fs.protected_regular and fs.protected_fifos
 * (`may_create_in_sticky`) before the checks of its kind, as on Linux.
 *
 * The descriptor is reserved before the path is resolved, as Linux reserves
 * it, so that no other thread sharing the table can take the last one while
 * the resolution sleeps and leave a created or truncated file behind an
 * EMFILE. A synthetic node, a device node and a socket file give the
 * reservation back, since their opens take their own descriptors and create
 * and truncate nothing; every error gives it back too.
 */
static int32_t do_open(const char *name, int flags, uint32_t mode)
{
    struct vfs_ref ref;
    struct vfs_stat dir;
    struct vfs_name *entry = NULL;
    struct vfs_mount *locked;
    char *through = NULL;
    int followed = 0;
    int fd;
    int rc;

    if (flags & O_PATH) {
        return open_path(name, flags);
    }
    if (open_device_name(name, flags, &rc)) {
        return rc;
    }
    fd = fd_alloc(current_proc->fds);
    if (fd < 0) {
        return fd;
    }
again:
    rc = vfs_resolve_entry(name,
                           VFS_WALK_LOCK |
                               (((flags & O_NOFOLLOW) || ((flags & O_CREAT) && (flags & O_EXCL)))
                                    ? VFS_WALK_NOFOLLOW
                                    : 0),
                           &ref, NULL, &entry, &dir);
    if (rc == -ENOENT && (flags & O_CREAT)) {
        if (!through) {
            through = path_buf_get();
            if (!through) {
                rc = -ENOMEM;
                goto out;
            }
        }
        rc = open_link_target(name, through, VFS_PATH_MAX, &followed);
        if (rc < 0) {
            goto out;
        }
        if (rc > 0) {
            name = through;
            goto again;
        }
        rc = open_create(name, flags, mode & 07777u, &ref, &entry);
        if (rc > 0) {
            goto again;
        }
        if (rc != 0) {
            goto out;
        }
        flags &= ~O_TRUNC;
        fd_slot(current_proc->fds, fd)->ofd->watch_flags = OFD_WATCH_MADE;
    } else if (rc != 0) {
        goto out;
    } else {
        const struct vpath *vp = vfs_synth_vpath(ref.mnt, ref.node);

        if (vp) {
            struct vpath vpc = *vp;
            int ro = proc_read_only(&ref, flags, 0);
            int permanent = mount_permanent_at(ref.mnt);

            vfs_walk_unlock(ref.mnt);
            fd_free(current_proc->fds, fd);
            fd = -1;
            rc = ro ? -EROFS : open_synth_node(&vpc, flags);
            synth_mount_record(rc, &ref, &vpc, flags);
            vfs_ref_release(&ref);
            synth_dir_record(rc, permanent, name);
            goto out;
        }
        rc = (flags & (O_CREAT | O_EXCL)) == O_CREAT ? may_create_in_sticky(&ref, &dir) : 0;
        if (rc != 0) {
            vfs_walk_unlock(ref.mnt);
            vfs_ref_release(&ref);
            goto out;
        }
        if (open_special(&ref, fd, flags, &rc)) {
            fd = -1;
            goto out;
        }
        locked = ref.mnt;
        rc = mnt_is_root(ref.mnt) ? open_root_node(&ref, flags) : open_mounted_node(&ref, flags);
        if (rc != 0) {
            vfs_walk_unlock(locked);
            goto out;
        }
    }
    locked = ref.mnt;
    fd_slot(current_proc->fds, fd)->ofd->name = entry;
    entry = NULL;
    rc = open_install(&ref, fd, name, flags);
    fd = -1;
    vfs_walk_unlock(locked);
out:
    vfs_name_put(entry);
    if (fd >= 0) {
        fd_free(current_proc->fds, fd);
    }
    path_buf_put(through);
    return rc;
}

#define MS_REMOUNT 32
#define MS_BIND    0x00001000u

/* The propagation flags, which mount(2) takes alone, with MS_REC. */
#define MS_REC        0x00004000u
#define MS_UNBINDABLE 0x00020000u
#define MS_PRIVATE    0x00040000u
#define MS_SLAVE      0x00080000u
#define MS_SHARED     0x00100000u

/** Whether the file system type `type` is `name`. */
static int type_is(const char *type, const char *name)
{
    while (*type && *type == *name) {
        type++;
        name++;
    }
    return *type == *name;
}

/**
 * Whether the caller may mount or unmount a file system of type `type`
 * (NULL for an unmount or a change of propagation): privileged, or
 * privileged over its mount namespace, where the types a user namespace
 * may mount on Linux are proc, sysfs, tmpfs, mqueue and devpts.
 */
static int mount_privileged(const char *type)
{
    struct mntns *ns = proc_mntns(current_proc);

    if (cred_privileged()) {
        return 1;
    }
    if (ns == &init_mntns || !ns_owner_privileged(current_proc, &ns->c)) {
        return 0;
    }
    return !type || type_is(type, "proc") || type_is(type, "sysfs") || type_is(type, "tmpfs") ||
           type_is(type, "mqueue") || type_is(type, "devpts");
}

/* The flags a remount without privilege may set but not clear. */
#define REMOUNT_LOCKED (VFS_MS_RDONLY | VFS_MS_NOSUID | VFS_MS_NODEV | VFS_MS_NOEXEC)

/**
 * mount(2) with MS_REMOUNT: the mount whose root `path` names takes the
 * flags `flags`; -EINVAL for a path that is no mount's root. With `bind`,
 * for MS_BIND, that mount alone (`vfs_remount_bind`); without, the file
 * system it shows, through every mount that shows it, with the options of
 * `data` (`vfs_remount`). A mount made read-only must have no descriptor
 * open for writing through what the change makes read-only, -EBUSY, as on
 * Linux. A caller without privilege, which `sys_mount` let through as
 * privileged over its mount namespace, may change only a bind that no other
 * live namespace sees, and may not clear REMOUNT_LOCKED there, -EPERM.
 */
static int32_t remount_at(const char *path, uint32_t flags, int bind, const char *data)
{
    struct vfs_ref ref;
    struct vfs_mount *m;
    int rc = vfs_resolve(0, path, 0, &ref, 0, 0);

    if (rc != 0) {
        return rc;
    }
    if (ref.node != ref.mnt->root) {
        vfs_ref_release(&ref);
        return -EINVAL;
    }
    m = bind || !ref.mnt->bind_of ? ref.mnt : ref.mnt->bind_of;
    if (!cred_privileged() &&
        (!bind || vfs_mount_shared(m) || (m->flags & REMOUNT_LOCKED & ~flags))) {
        vfs_ref_release(&ref);
        return -EPERM;
    }
    rc = bind ? vfs_remount_bind(m, flags) : vfs_remount(m, flags, data);
    vfs_ref_release(&ref);
    return rc;
}

/** mount(2) with MS_REMOUNT: the target's path and, without MS_BIND, the
 * data, copied from the caller for `remount_at`. */
static int32_t remount_call(const isr_params_t *params)
{
    char data[128];
    char *tpath;
    int32_t n;

    if (!SC_ARG(params, 1)) {
        return -EFAULT;
    }
    data[0] = '\0';
    if (SC_ARG(params, 4) && !(SC_ARG(params, 3) & MS_BIND)) {
        n = strncpy_from_user(data, (const char __user *)SC_ARG(params, 4), sizeof(data));
        if (n < 0) {
            return n;
        }
    }
    n = path_from_user(AT_FDCWD, (const char __user *)SC_ARG(params, 1), &tpath);
    if (n == 0) {
        n = remount_at(tpath, SC_ARG(params, 3) & ~(uint32_t)(MS_REMOUNT | MS_BIND),
                       (SC_ARG(params, 3) & MS_BIND) != 0, data);
        path_buf_put(tpath);
    }
    return n;
}

/** mount(2) with MS_BIND: the source is a path, as the target is, and MS_REC
 * shows the mounts below it too (`vfs_do_bind`). 0 or a negative errno. */
static int32_t bind_at(uintptr_t source, const char *target, uint32_t flags)
{
    char *spath;
    int rc = path_from_user(AT_FDCWD, (const char __user *)source, &spath);

    if (rc == 0) {
        rc = vfs_do_bind(spath, target, (flags & MS_REC) != 0);
        path_buf_put(spath);
    }
    return rc;
}

/**
 * mount(2) takes source, target, type, flags and data, and returns 0 or a
 * negative errno.
 *
 * The caller must have privilege, or privilege over its mount namespace for
 * a bind, for proc, sysfs, tmpfs, mqueue and devpts (`mount_privileged`),
 * and for a remount of a bind's flags that only its namespace sees and that
 * clears none of read-only, nosuid, nodev and noexec (`remount_at`), or the
 * call is -EPERM; a remount of a file system needs the privilege itself, as
 * on Linux, where those four flags of a mount a user namespace was given
 * cannot be cleared there. The strings are
 * copied here, the target made an absolute canonical path, a relative one
 * joined to the working directory as every call that takes a path joins it;
 * the order of work and every other refusal are the VFS's (vfs_do_mount).
 * `data` may be NULL, which is an empty option string. With MS_REMOUNT the
 * source and the type are not read, as on Linux, and the data goes to the
 * file system, which takes the options a remount may change (`remount_at`);
 * with MS_BIND alone the type and the data are not read (`bind_at`). A
 * change of propagation alone, MS_PRIVATE, MS_SLAVE, MS_SHARED or
 * MS_UNBINDABLE with MS_REC or without, changes nothing: every mount is
 * private to the namespaces that see it, as unshare(1) makes them by
 * default.
 */
long sys_mount(isr_params_t *params)
{
    char *tpath;
    char source[64];
    char type[16];
    char data[128];
    int n;

    if (!current_proc) {
        return -ESRCH;
    }
    if ((SC_ARG(params, 3) & (MS_PRIVATE | MS_SLAVE | MS_SHARED | MS_UNBINDABLE)) &&
        !(SC_ARG(params, 3) &
          ~(MS_REC | MS_PRIVATE | MS_SLAVE | MS_SHARED | MS_UNBINDABLE | VFS_MS_SILENT))) {
        return mount_privileged(NULL) ? 0 : -EPERM;
    }
    if ((SC_ARG(params, 3) & MS_REMOUNT) && !(SC_ARG(params, 3) & MS_BIND)
            ? !cred_privileged()
            : !mount_privileged(NULL)) {
        return -EPERM;
    }
    if (SC_ARG(params, 3) & MS_REMOUNT) {
        return remount_call(params);
    }
    if (SC_ARG(params, 3) & MS_BIND) {
        if (!SC_ARG(params, 0) || !SC_ARG(params, 1)) {
            return -EFAULT;
        }
        n = path_from_user(AT_FDCWD, (const char __user *)SC_ARG(params, 1), &tpath);
        if (n == 0) {
            n = bind_at(SC_ARG(params, 0), tpath, (uint32_t)SC_ARG(params, 3));
            path_buf_put(tpath);
        }
        return n;
    }
    if (!SC_ARG(params, 0) || !SC_ARG(params, 1) || !SC_ARG(params, 2)) {
        return -EFAULT;
    }
    n = strncpy_from_user(source, (const char __user *)SC_ARG(params, 0), sizeof(source));
    if (n < 0) {
        return n;
    }
    n = path_from_user(AT_FDCWD, (const char __user *)SC_ARG(params, 1), &tpath);
    if (n != 0) {
        return n;
    }
    n = strncpy_from_user(type, (const char __user *)SC_ARG(params, 2), sizeof(type));
    if (n >= 0 && !mount_privileged(type)) {
        path_buf_put(tpath);
        return -EPERM;
    }
    data[0] = '\0';
    if (n >= 0 && SC_ARG(params, 4)) {
        n = strncpy_from_user(data, (const char __user *)SC_ARG(params, 4), sizeof(data));
    }
    if (n >= 0) {
        n = vfs_do_mount(source, tpath, type, (uint32_t)SC_ARG(params, 3), data);
    }
    path_buf_put(tpath);
    return n;
}

/**
 * Take `root`, a directory's path from the root of the mount table, off the
 * front of `path`, another such path, which leaves the path the same place
 * has from that directory: "/" for the directory itself. 0, or -ENOENT when
 * `path` is not at or below `root`.
 */
static int32_t path_strip(char *path, const char *root)
{
    uint32_t rl = (uint32_t)k_strlen(root);

    if (!path_under(path, root, rl)) {
        return -ENOENT;
    }
    if (rl > 1) {
        k_memmove(path, path + rl, k_strlen(path + rl) + 1);
        if (path[0] == '\0') {
            path[0] = '/';
            path[1] = '\0';
        }
    }
    return 0;
}

/* umount2(2)'s flag for a lazy unmount, the one flag this kernel takes. */
#define MNT_DETACH 2u

/**
 * umount2(2) takes target and flags, and returns 0 or a negative errno. The
 * caller must have privilege, or privilege over its mount namespace
 * (`mount_privileged`), or the call is -EPERM. The flags are 0 or
 * MNT_DETACH, which takes the mount and those below it from the caller's
 * view at once, however busy, and unmounts them once nothing holds them
 * (`vfs_do_detach`); this kernel has no MNT_FORCE, MNT_EXPIRE or
 * UMOUNT_NOFOLLOW, and answers EINVAL to them.
 *
 * A target that names a directory by its end must reach one, which the walk
 * that finds the mount checks (`vfs_do_umount`), so a file named with a
 * trailing slash is -ENOTDIR, as on Linux, where a file named without one is
 * not a mount point, -EINVAL. The last "." put back comes off the target
 * first: a walk that meets a removed device answers -EIO, which
 * `vfs_do_umount` recovers from by the recorded target, and a recorded
 * target has none.
 */
long sys_umount2(isr_params_t *params)
{
    char *tpath;
    int end;
    int n;

    if (!current_proc) {
        return -ESRCH;
    }
    if (!mount_privileged(NULL)) {
        return -EPERM;
    }
    if ((SC_ARG(params, 1) & ~(unsigned long)MNT_DETACH) != 0) {
        return -EINVAL;
    }
    if (!SC_ARG(params, 0)) {
        return -EFAULT;
    }
    n = path_from_user_dir(AT_FDCWD, (const char __user *)SC_ARG(params, 0), &tpath, &end);
    if (n != 0) {
        return n;
    }
    path_drop_dot(tpath, end);
    n = SC_ARG(params, 1) ? vfs_do_detach(tpath, end != 0) : vfs_do_umount(tpath, end != 0);
    path_buf_put(tpath);
    return n;
}

/**
 * Move the root of `q`, the caller or a process that shares its mount
 * namespace and its old root, to `nref`, after pivot_root: its working
 * directory's path, which is a path from its root, becomes the same place's
 * path from the new root, or the path from the root of the mount table an
 * outside one keeps (`cwd_outside`); a working directory at the old root
 * goes to the new one, as on Linux. `oldroot` is the old root's path from
 * the root of the mount table, and `buf` a path page. The path and the root
 * are stored with nothing between them that sleeps.
 */
static void pivot_one(process_t *q, const struct vfs_ref *nref, const char *oldroot, char *buf)
{
    const char *cwd = proc_cwd(q);
    const char *newroot = nref->mnt->covered_path;
    uint32_t ol = oldroot[1] == '\0' ? 0u : (uint32_t)k_strlen(oldroot);
    uint32_t cl = (uint32_t)k_strlen(cwd);
    int home = cwd[0] == '/' && cwd[1] == '\0';

    if (cwd_outside(cwd)) {
        k_memcpy(buf, cwd + 1, cl);
    } else if (ol + cl + 1 <= VFS_PATH_MAX) {
        k_memcpy(buf, oldroot, ol);
        k_memcpy(buf + ol, cwd, cl + 1);
    } else {
        home = 1;
    }
    if (!home && path_strip(buf, newroot) != 0) {
        cl = (uint32_t)k_strlen(buf);
        home = cl + 2 > VFS_PATH_MAX;
        if (!home) {
            k_memmove(buf + 1, buf, cl + 1);
        }
    }
    if (home || proc_cwd_set(q, buf) != 0) {
        home = 1;
        (void)proc_cwd_set(q, "/");
    }
    vfs_root_set(q, nref->mnt, nref->node);
    if (home) {
        vfs_cwd_set(q, nref->mnt, nref->node);
    }
}

/** The first process of the mount namespace `ns` whose root is still the
 * old root, the root of `omnt` at `onode`, NULL for the machine's; or NULL
 * when none is. The search starts again after each move, since a move may
 * sleep and the list change meanwhile. */
static process_t *pivot_next(const struct mntns *ns, const struct vfs_mount *omnt,
                             const void *onode)
{
    process_t *q;

    for (q = proc_list; q; q = q->next) {
        if (q->state != PROC_UNUSED && q->state != PROC_ZOMBIE && q->fs && proc_mntns(q) == ns &&
            q->fs->root_mnt == omnt && q->fs->root_node == onode) {
            return q;
        }
    }
    return NULL;
}

/**
 * pivot_root(2) takes new_root and put_old: the root of a mount becomes the
 * caller's root, and the old root is shown at put_old, a directory at or
 * below new_root (`vfs_pivot_mounts`). Every process of the caller's mount
 * namespace whose root was the old root has the new one after, the caller
 * among them, and its working directory keeps its place (`pivot_one`). The
 * caller needs privilege over its mount namespace (`mount_privileged`), or
 * the call is -EPERM.
 *
 * With put_old naming new_root itself, as bubblewrap, runc and LXC call it
 * with ".", ".", the old root is not shown anywhere: on Linux it is stacked
 * over the new one, and the caller lets go of it by a descriptor opened on
 * it before, fchdir, and umount2 of "." with MNT_DETACH. Returns 0, -EFAULT,
 * -EPERM, -ENOMEM, or the errors of `vfs_pivot_mounts`.
 */
long sys_pivot_root(isr_params_t *params)
{
    const struct mntns *ns;
    struct vfs_mount *omnt;
    void *onode;
    struct vfs_ref nref;
    char *npath, *opath, *oldroot, *buf;
    process_t *q;
    int32_t rc;

    if (!current_proc) {
        return -ESRCH;
    }
    if (!mount_privileged(NULL)) {
        return -EPERM;
    }
    if (!SC_ARG(params, 0) || !SC_ARG(params, 1)) {
        return -EFAULT;
    }
    ns = proc_mntns(current_proc);
    rc = path_from_user(AT_FDCWD, (const char __user *)SC_ARG(params, 0), &npath);
    if (rc != 0) {
        return rc;
    }
    rc = path_from_user(AT_FDCWD, (const char __user *)SC_ARG(params, 1), &opath);
    if (rc != 0) {
        path_buf_put(npath);
        return rc;
    }
    oldroot = path_buf_get();
    buf = path_buf_get();
    omnt = current_proc->fs->root_mnt;
    onode = current_proc->fs->root_node;
    rc = oldroot && buf ? 0 : -ENOMEM;
    if (rc == 0 && omnt) {
        rc = vfs_node_path(omnt, onode, oldroot, VFS_PATH_MAX);
    } else if (rc == 0) {
        k_strncpy(oldroot, "/", VFS_PATH_MAX);
    }
    if (rc == 0) {
        rc = vfs_pivot_mounts(npath, opath, &nref);
    }
    while (rc == 0 && (q = pivot_next(ns, omnt, onode)) != NULL) {
        pivot_one(q, &nref, oldroot, buf);
    }
    if (rc == 0) {
        vfs_ref_release(&nref);
    }
    path_buf_put(buf);
    path_buf_put(oldroot);
    path_buf_put(opath);
    path_buf_put(npath);
    return rc;
}

/**
 * The removal rule for the name `leaf` in the directory `parent` of a type
 * other than the KRFFS root, which has its own copy of the rule beside its
 * node fields (`cred_sticky_ok`): write permission on the directory, and in
 * a sticky directory the entry's owner, the directory's owner, or a
 * privileged caller. A name the directory does not have passes, since no one
 * owns it. Returns 0, -EACCES, -EPERM, or the type's error.
 */
static int cred_remove_ref(const struct vfs_ref *parent, const char *leaf)
{
    struct vfs_stat pst, est;
    void *node;
    int rc = cred_access_ref(parent, CRED_W, 0);

    if (rc != 0 || cred_fs_privileged()) {
        return rc;
    }
    rc = vfs_getattr(parent->mnt, parent->node, &pst);
    if (rc != 0 || !(pst.mode & 01000u) || pst.uid == proc_fsuid(current_proc)) {
        return rc;
    }
    rc = parent->mnt->type->ops->lookup(parent->mnt, parent->node, leaf, &node);
    if (rc != 0) {
        return rc == -ENOENT ? 0 : rc;
    }
    rc = vfs_getattr(parent->mnt, node, &est);
    if (parent->mnt->type->ops->put) {
        parent->mnt->type->ops->put(parent->mnt, node);
    }
    if (rc != 0) {
        return rc;
    }
    return est.uid == proc_fsuid(current_proc) ? 0 : -EPERM;
}

/**
 * Find, for inotify, the directory the path `name` is in and its last
 * component, when that component names the file `ino` of `mnt`: copy the
 * component to `leaf`, of VFS_NAME_MAX + 1 bytes, and the directory's inode
 * number to `*dir_ino`, and return 0; -1 otherwise. A name whose last
 * component is a link the call followed finds nothing, where Linux would
 * report to the target's directory.
 */
static int named_parent(const char *name, const struct vfs_mount *mnt, uint64_t ino, char *leaf,
                        uint64_t *dir_ino)
{
    struct vfs_ref parent;
    struct vfs_stat dir, st;
    int rc = -1;
    void *found;

    if (vfs_resolve(0, name, VFS_WALK_PARENT, &parent, leaf, VFS_NAME_MAX + 1) != 0) {
        return -1;
    }
    if (parent.mnt == mnt && leaf[0] != '\0' && !leaf_is_dot(leaf) &&
        vfs_getattr(parent.mnt, parent.node, &dir) == 0 && parent.mnt->type->ops->lookup &&
        parent.mnt->type->ops->lookup(parent.mnt, parent.node, leaf, &found) == 0) {
        if (vfs_getattr(parent.mnt, found, &st) == 0 && st.ino == ino) {
            *dir_ino = dir.ino;
            rc = 0;
        }
        if (parent.mnt->type->ops->put) {
            parent.mnt->type->ops->put(parent.mnt, found);
        }
    }
    vfs_ref_release(&parent);
    return rc;
}

/**
 * Record in `ofd`, for inotify, the directory the open's `name` is in and
 * its last component, when they are the file opened (`named_parent`).
 */
static void open_record_parent(struct open_file *ofd, const char *name)
{
    char leaf[VFS_NAME_MAX + 1];
    uint64_t dir_ino;
    uint32_t len;

    if (named_parent(name, ofd->vmnt, ofd->ino, leaf, &dir_ino) != 0) {
        return;
    }
    len = (uint32_t)k_strlen(leaf);
    ofd->leaf = kmalloc(len + 1);
    if (ofd->leaf) {
        k_memcpy(ofd->leaf, leaf, len + 1);
        ofd->parent_ino = dir_ino;
    }
}

/**
 * Tell inotify of the change `mask` to the attributes of the file (`mnt`,
 * `node`), which the path `name` named: the watches of its directory hear of
 * it under its last component, as Linux's `fsnotify_parent` tells them, and
 * with `self` the file's own watches as well, for a change that did not
 * pass through `vfs_setattr`, which tells those. Nothing while there is no
 * watch, or for a mask of no event. The caller holds a reference to the
 * node and none of the walk's locks.
 */
static void attr_told(const char *name, struct vfs_mount *mnt, void *node, uint32_t mask, int self)
{
    char leaf[VFS_NAME_MAX + 1];
    struct vfs_stat st;
    uint64_t dir_ino;

    if (!inotify_watches || mask == 0 || vfs_getattr(mnt, node, &st) != 0) {
        return;
    }
    if (self) {
        inotify_file(mnt, st.ino, mask);
    }
    if (named_parent(name, mnt, st.ino, leaf, &dir_ino) == 0) {
        inotify_child(mnt, dir_ino, leaf, mask | ((st.mode & S_IFMT) == S_IFDIR ? IN_ISDIR : 0u),
                      0);
    }
}

/**
 * `attr_told` for a change made through the description `ofd`: its
 * directory's watches hear of it when its open recorded the directory and
 * name (`open_record_parent`), and with `self` its own watches as well.
 */
static void attr_told_desc(struct open_file *ofd, uint32_t mask, int self)
{
    if (!inotify_watches || mask == 0 || !ofd || !ofd->vmnt) {
        return;
    }
    if (self) {
        inotify_file(ofd->vmnt, ofd->ino, mask);
    }
    if (ofd->leaf) {
        inotify_child(ofd->vmnt, ofd->parent_ino, ofd->leaf,
                      mask | ((ofd->watch_flags & OFD_WATCH_DIR) ? IN_ISDIR : 0u), 0);
    }
}

/**
 * What the open that made the descriptor `fd` from `name`, leading '/'
 * stripped, tells the shared mappings of the file, which lose the pages an
 * O_TRUNC truncation freed, and inotify: the open, after that truncation of
 * a file the open did not create.
 *
 * The description records whether it was opened for writing, which its
 * close reports, and, while a watch exists, its directory and name
 * (`open_record_parent`), so that the directory's watches hear of what is
 * done through it.
 */
static void open_done(int fd, const char *name, int flags)
{
    fd_entry_t *e = fd_get(current_proc->fds, fd);
    struct open_file *ofd = e ? e->ofd : NULL;
    int truncated;

    if (!ofd || !ofd->vmnt || (e->flags & FD_FLAG_PATH)) {
        return;
    }
    truncated = (flags & O_TRUNC) && !(ofd->watch_flags & OFD_WATCH_MADE) &&
                (e->flags & FD_FLAG_WRITE) && e->type == FD_TYPE_FILE;
    ofd->watch_flags = OFD_WATCH_OPEN | ((e->flags & FD_FLAG_WRITE) ? OFD_WATCH_WRITE : 0u) |
                       (e->type == FD_TYPE_DIR ? OFD_WATCH_DIR : 0u);
    if (truncated && !vfs_file_locked(ofd->vmnt)) {
        filemap_file_truncated(ofd->vmnt, ofd->ino, 0); /* else under the file's lock */
    }
    if (!inotify_mount_watched(ofd->vmnt)) {
        return;
    }
    open_record_parent(ofd, name);
    if (truncated) {
        inotify_desc(ofd, IN_MODIFY);
    }
    inotify_desc(ofd, IN_OPEN);
}

/* The open flags that send an open to the big lock (`open_unlocked`): any
 * access mode but O_RDONLY, the low two bits, and every flag that creates,
 * truncates, opens nothing or opens a nameless file (Linux's __O_TMPFILE). */
#define OPEN_LOCKED_FLAGS (3 | O_CREAT | O_TRUNC | O_PATH | 020000000)

/**
 * The descriptor of an open that `open_unlocked` checked and holds `ref`
 * and `entry` for, in the slot `fd` it reserved, made as `open_install`
 * makes one for a read-only open of a mounted type: the description takes
 * the hold, the record and a reference on the mount, and the entry is
 * published last. What `open_done` does for such an open, the description's
 * watch flags and inotify's report of the open, under the big lock while a
 * watch is on the file's file system (`inotify_mount_watched`), is done to
 * the description before the entry is published, as Linux's `vfs_open`
 * reports the open before `fd_install` (fs/open.c): once published, another
 * thread may close the descriptor and open another file in its slot. The
 * descriptor, or a negative errno with everything given back.
 */
static int32_t open_install_unlocked(struct vfs_ref *ref, struct vfs_name *entry, int fd,
                                     const char *name, int flags, const struct vfs_stat *st)
{
    fd_entry_t *e = fd_slot(current_proc->fds, fd);
    int rc;

    e->ofd->name = entry;
    e->ofd->ino = st->ino;
    e->ofd->offset = 0;
    ofd_record_path(e->ofd, name);
    rc = vfs_open_node(ref->mnt, ref->node, flags, &e->ofd->fhandle);
    if (rc != 0) {
        fd_free(current_proc->fds, fd);
        vfs_ref_release(ref);
        return rc;
    }
    e->ofd->vmnt = ref->mnt;
    vfs_wb_err_sample(ref->mnt, &e->ofd->wb_seen, &e->ofd->sb_seen);
    e->ofd->ino_seen = vfs_ino_err_sample(ref->mnt, e->ofd->ino);
    e->ofd->vnode = ref->node;
    vfs_desc_open(ref->mnt);
    vfs_leave(ref->mnt);
    fd_ofd_access_drop(e->ofd);
    e->flags = FD_FLAG_READ | ((flags & O_CLOEXEC) ? FD_FLAG_CLOEXEC : 0);
    e->ofd->status_flags |= OFD_OPEN_STATUS(flags);
    e->node = NULL;
    e->type = (st->mode & S_IFMT) == S_IFDIR ? FD_TYPE_DIR : FD_TYPE_FILE;
    e->ofd->watch_flags = OFD_WATCH_OPEN | (e->type == FD_TYPE_DIR ? OFD_WATCH_DIR : 0u);
    if (inotify_mount_watched(ref->mnt)) {
        bkl_enter();
        open_record_parent(e->ofd, name);
        inotify_desc(e->ofd, IN_OPEN);
        bkl_exit();
    }
    fd_install(current_proc->fds, fd);
    return fd;
}

/**
 * `open_from_user` for a caller without the big lock: a read-only open that
 * creates and truncates nothing, of a regular file or a directory on a type
 * that sets `reads_unlocked`, whose path the name cache finishes
 * (`vfs_resolve_cached`), absolute or relative to the working directory
 * (`path_from_user_unlocked`), as Linux's `path_openat` walks under RCU
 * first and again with references when that fails (fs/namei.c:4055-4057,
 * `do_filp_open`). The checks are those `open_mounted_node` makes for such
 * an open: ENOTDIR for O_DIRECTORY on a file, and read permission. The
 * descriptor is reserved before the walk, as `do_open` reserves it. The
 * descriptor or a negative errno; or CALL_LOCKED with nothing done, for
 * every other open, which `do_open` makes under the big lock. inotify's
 * report of the open takes the big lock while a watch is on the file's
 * file system (`open_install_unlocked`).
 */
static int32_t open_unlocked(int dirfd, const char __user *upath, int flags)
{
    struct vfs_name *entry = NULL;
    struct vfs_stat st;
    struct vfs_ref ref;
    const char *name;
    char *path;
    int end = 0, fd;
    int32_t rc;

    if (flags & OPEN_LOCKED_FLAGS) {
        return CALL_LOCKED;
    }
    rc = path_from_user_unlocked(dirfd, upath, &path, &end);
    if (rc != 0) {
        return rc;
    }
    if (end) {
        flags = (flags | O_DIRECTORY) & ~O_NOFOLLOW;
    }
    name = path_name(path);
    if (device_name(name, "fb") || device_name(name, "mouse") || path_starts(name, "proc/") ||
        path_starts(name, "dev/")) {
        path_buf_put(path);
        return CALL_LOCKED;
    }
    fd = fd_alloc(current_proc->fds);
    if (fd < 0) {
        path_buf_put(path);
        return fd;
    }
    rc = vfs_resolve_cached(path, VFS_WALK_LOCK | ((flags & O_NOFOLLOW) ? VFS_WALK_NOFOLLOW : 0),
                            &ref, &entry);
    if (rc != 0) {
        fd_free(current_proc->fds, fd);
        path_buf_put(path);
        return rc == VFS_CACHED_MISS ? CALL_LOCKED : rc;
    }
    if (!vfs_mount_base(ref.mnt)->type->reads_unlocked ||
        vfs_getattr(ref.mnt, ref.node, &st) != 0 ||
        ((st.mode & S_IFMT) != S_IFREG && (st.mode & S_IFMT) != S_IFDIR)) {
        rc = CALL_LOCKED;
    } else if ((st.mode & S_IFMT) != S_IFDIR && (flags & O_DIRECTORY)) {
        rc = -ENOTDIR;
    } else {
        rc = cred_access_ref(&ref, CRED_R, 0);
    }
    if (rc != 0) {
        vfs_name_put(entry);
        vfs_ref_release(&ref);
        fd_free(current_proc->fds, fd);
        path_buf_put(path);
        return rc;
    }
    rc = open_install_unlocked(&ref, entry, fd, name, flags, &st);
    path_buf_put(path);
    return rc;
}

/**
 * open(2) and openat(2) after their arguments: the path `upath`, a relative
 * one resolved from `dirfd`, opened by `open_named` with the Linux open
 * `flags` and the permission bits `mode` of a file O_CREAT makes.
 *
 * O_CREAT with O_DIRECTORY asks for a file and for a directory at once, and
 * is -EINVAL before the path is read, as Linux answers it since 6.4; an
 * older Linux made a regular file. O_PATH, which drops O_CREAT, takes the
 * pair, as Linux's open and openat do.
 *
 * A caller without the big lock tries `open_unlocked` first and takes the
 * big lock for what that leaves.
 */
static int32_t open_from_user(int dirfd, const char __user *upath, int flags, uint32_t mode)
{
    char *path;
    int end, big;
    int32_t rc;

    if ((flags & (O_CREAT | O_DIRECTORY | O_PATH)) == (O_CREAT | O_DIRECTORY)) {
        return -EINVAL;
    }
    if (!upath) {
        return -EFAULT;
    }
    big = !bkl_held();
    if (big) {
        rc = open_unlocked(dirfd, upath, flags);
        if (rc != CALL_LOCKED) {
            return rc;
        }
        bkl_enter();
    }
    rc = path_from_user_dir(dirfd, upath, &path, &end);
    if (rc == 0) {
        rc = open_named(path_name(path), flags, mode, end);
        if (rc >= 0) {
            open_done(rc, path_name(path), flags);
        }
        path_buf_put(path);
    }
    if (big) {
        bkl_exit();
    }
    return rc;
}

/** open(2) takes path, flags and the O_CREAT mode; the descriptor, or -errno
 * (`open_from_user`). */
long sys_open(isr_params_t *params)
{
    if (!current_proc) {
        return -EFAULT;
    }
    return open_from_user(AT_FDCWD, (const char __user *)SC_ARG(params, 0), (int)SC_ARG(params, 1),
                          (uint32_t)SC_ARG(params, 2));
}

/**
 * openat takes dirfd, path, flags and the O_CREAT mode; the descriptor, or
 * -errno (`open_from_user`). musl's openat() issues it; its open() and
 * <stdio.h> use open(2), whose number i386 and x86-64 both have. An absolute
 * path is taken as-is, a relative path resolves against the cwd (AT_FDCWD)
 * or against the directory the dirfd refers to (see copy_path_at).
 */
long sys_openat(isr_params_t *params)
{
    if (!current_proc) {
        return -EFAULT;
    }
    return open_from_user((int)SC_ARG(params, 0), (const char __user *)SC_ARG(params, 1),
                          (int)SC_ARG(params, 2), (uint32_t)SC_ARG(params, 3));
}

/* openat2(2)'s struct open_how, 24 bytes in both ABIs, and the flags it
 * checks that open(2) passes over. */
struct open_how {
    uint64_t flags;
    uint64_t mode;
    uint64_t resolve;
};

#define OPEN_HOW_SIZE_VER0   24u
#define O_ACCMODE            3
#define O_LARGEFILE          0100000
#define O_TMPFILE_BIT        020000000 /* Linux's __O_TMPFILE, which O_TMPFILE pairs with O_DIRECTORY */
/* Every flag open(2) has, O_ACCMODE's two bits through __O_TMPFILE, and
 * Linux 6.12's FMODE_NONOTIFY, which openat2 drops unseen. */
#define OPENAT2_FLAGS        037777703ull
#define OPENAT2_NONOTIFY     01000000000ull
#define OPENAT2_PATH_FLAGS   (O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)
#define OPENAT2_RESOLVE      0x3Full
#define OPENAT2_RESOLVE_ROOT (VFS_RESOLVE_BENEATH | VFS_RESOLVE_IN_ROOT)

/**
 * Read the caller's struct open_how of `usize` bytes at `uhow` into `how`,
 * as Linux's `copy_struct_from_user` does: -EINVAL for a size below the
 * first version's 24 bytes, -E2BIG past a page or for a byte past the 24
 * this kernel knows that is not zero, -EFAULT for memory the caller may not
 * read.
 */
static int32_t open_how_read(uintptr_t uhow, unsigned long usize, struct open_how *how)
{
    uint8_t tail[64];
    unsigned long at;

    if (usize < OPEN_HOW_SIZE_VER0) {
        return -EINVAL;
    }
    if (usize > PAGE_SIZE) {
        return -E2BIG;
    }
    for (at = OPEN_HOW_SIZE_VER0; at < usize;) {
        unsigned long n = usize - at < sizeof(tail) ? usize - at : sizeof(tail);
        unsigned long i;

        if (copy_from_user(tail, (const void __user *)(uhow + at), n) != 0) {
            return -EFAULT;
        }
        for (i = 0; i < n; i++) {
            if (tail[i] != 0) {
                return -E2BIG;
            }
        }
        at += n;
    }
    return copy_from_user(how, (const void __user *)uhow, sizeof(*how)) != 0 ? -EFAULT : 0;
}

/**
 * Check `how` as Linux's `build_open_flags` checks openat2's arguments, in
 * its order, and return the open flags, or -EINVAL: for a flag open(2) does
 * not have, a RESOLVE_ flag Linux does not have, BENEATH with IN_ROOT, a
 * mode with a bit above 07777 or with neither O_CREAT nor O_TMPFILE, O_CREAT
 * with O_DIRECTORY, O_TMPFILE without O_DIRECTORY or write access, and
 * O_PATH with a flag but O_DIRECTORY, O_NOFOLLOW and O_CLOEXEC; -EAGAIN for
 * RESOLVE_CACHED with O_CREAT, O_TRUNC or O_TMPFILE, which would write.
 * Where Linux's 64-bit kernel adds O_LARGEFILE to an open but O_PATH's, so
 * does the x86-64 kernel's, for a 32-bit program too.
 */
static int32_t open_how_flags(const struct open_how *how)
{
    uint64_t flags = how->flags & ~OPENAT2_NONOTIFY;

#ifdef __x86_64__
    if (!(flags & O_PATH)) {
        flags |= O_LARGEFILE;
    }
#endif
    if ((flags & ~OPENAT2_FLAGS) || (how->resolve & ~OPENAT2_RESOLVE) ||
        (how->resolve & OPENAT2_RESOLVE_ROOT) == OPENAT2_RESOLVE_ROOT) {
        return -EINVAL;
    }
    if ((flags & (O_CREAT | O_TMPFILE_BIT)) ? (how->mode & ~07777ull) != 0 : how->mode != 0) {
        return -EINVAL;
    }
    if ((flags & (O_DIRECTORY | O_CREAT)) == (O_DIRECTORY | O_CREAT)) {
        return -EINVAL;
    }
    if ((flags & O_TMPFILE_BIT) && (!(flags & O_DIRECTORY) || (flags & O_ACCMODE) == 0)) {
        return -EINVAL;
    }
    if ((flags & O_PATH) && (flags & ~(uint64_t)OPENAT2_PATH_FLAGS)) {
        return -EINVAL;
    }
    if ((how->resolve & VFS_RESOLVE_CACHED) && (flags & (O_TRUNC | O_CREAT | O_TMPFILE_BIT))) {
        return -EAGAIN;
    }
    return (int32_t)flags;
}

/**
 * The directory an openat2 path is walked from, into `out`: that of the
 * descriptor `dirfd`, or the working directory for AT_FDCWD, with
 * `*outside` set when it is outside the caller's root, as `copy_path_at`
 * joins a relative name to it. 0, -EBADF, -ENOTDIR, or `fd_dir_path`'s
 * errors.
 */
static int32_t openat2_dir(int dirfd, char *out, int *outside)
{
    fd_entry_t *entry;
    int len;
    int r;

    *outside = 0;
    if (dirfd == AT_FDCWD) {
        const char *cwd = proc_cwd(current_proc);

        if (k_strlen(cwd) + 1 > VFS_PATH_MAX) {
            return -ENAMETOOLONG;
        }
        *outside = cwd_outside(cwd);
        k_memcpy(out, cwd, k_strlen(cwd) + 1);
    } else {
        entry = fd_get(current_proc->fds, dirfd);
        if (!entry) {
            return -EBADF;
        }
        r = fd_dir_path(entry, out, VFS_PATH_MAX, outside);
        if (r != 0) {
            return r;
        }
    }
    len = canonicalize_abs(out);
    if (*outside) {
        if ((uint32_t)len + 3 > VFS_PATH_MAX) {
            return -ENAMETOOLONG;
        }
        k_memmove(out + 2, out, (uint32_t)len + 1);
        out[1] = '/';
    }
    return 0;
}

/**
 * Finish the path `real`, `len` bytes, that an openat2's walk reached: the
 * missing name `leaf` the open may create after it when `rc` is
 * VFS_MISSING, and "//" before it when the walk began `outside` the
 * caller's root, as `copy_path_at` marks such a path. 0, or -ENAMETOOLONG.
 */
static int32_t openat2_real(char *real, uint32_t len, int rc, const char *leaf, int outside)
{
    uint32_t ll = (uint32_t)k_strlen(leaf);

    if (real[0] == '\0') {
        return -ENAMETOOLONG;
    }
    if (rc == VFS_MISSING) {
        if (len + 1 + ll + 1 > VFS_PATH_MAX) {
            return -ENAMETOOLONG;
        }
        if (len > 1) {
            real[len++] = '/';
        }
        k_memcpy(real + len, leaf, ll + 1);
        len += ll;
    }
    if (outside && len + 3 > VFS_PATH_MAX) {
        return -ENAMETOOLONG;
    }
    if (outside) {
        k_memmove(real + 2, real, len + 1);
        real[1] = '/';
    }
    return 0;
}

/**
 * Resolve the directory an openat2 path `name` under `resolve` starts at,
 * as the anchor of `scope`, with its real path into the path page
 * `anchor`: the directory `dirfd` names for a relative path or under
 * IN_ROOT, `*outside` set as `openat2_dir` sets it, and the caller's root
 * for any other absolute path. An absolute path under BENEATH is -EXDEV
 * before `dirfd` is read, as Linux's path_init answers it. The scope's
 * sequence is read before the anchor's walk, which may sleep, so that a
 * rename during that walk or the scoped walk after it trips the replay.
 * 0 with the anchor held, or an error.
 */
static int32_t openat2_anchor(int dirfd, const char *name, uint32_t resolve,
                              struct vfs_scope *scope, char *anchor, int *outside)
{
    uint32_t seq = vfs_tree_seq();
    char *dir;
    int32_t rc;

    if (name[0] == '/' && (resolve & VFS_RESOLVE_BENEATH)) {
        return -EXDEV;
    }
    dir = path_buf_get();
    rc = dir ? 0 : -ENOMEM;
    if (rc == 0 && (name[0] != '/' || (resolve & OPENAT2_RESOLVE_ROOT))) {
        rc = openat2_dir(dirfd, dir, outside);
    } else if (rc == 0) {
        dir[0] = '/';
        dir[1] = '\0';
    }
    if (rc == 0) {
        rc = vfs_resolve_real(path_name(dir), 0, &scope->anchor, anchor);
    }
    path_buf_put(dir);
    if (rc == 0 && anchor[0] == '\0') {
        vfs_ref_release(&scope->anchor);
        rc = -ENAMETOOLONG;
    }
    if (rc != 0) {
        return rc;
    }
    scope->resolve = resolve;
    scope->anchor_real = anchor;
    scope->anchor_len = anchor[1] == '\0' ? 0 : (uint32_t)k_strlen(anchor);
    scope->seq = seq;
    return 0;
}

/** Apply openat2's rules to a final magic link before the open follows it.
 * `vp` is NULL for an ordinary node; `mnt` is the link's held mount.
 * Return 0 or the link restriction's error. */
static int openat2_magic(const struct vpath *vp, struct vfs_mount *mnt, uint32_t resolve)
{
    process_t *owner;
    fd_entry_t src;
    int rc;

    if (!vp || (vp->kind != VNODE_PROC_FD_LINK && vp->kind != VNODE_PROC_NS_LINK)) {
        return 0;
    }
    if (resolve & (VFS_RESOLVE_NO_SYMLINKS | VFS_RESOLVE_NO_MAGICLINKS)) {
        return -ELOOP;
    }
    if (resolve & OPENAT2_RESOLVE_ROOT) {
        return -EXDEV;
    }
    if (!(resolve & VFS_RESOLVE_NO_XDEV)) {
        return 0;
    }
    if (vp->kind == VNODE_PROC_NS_LINK) {
        return -EXDEV;
    }
    rc = proc_fd_entry(vp, &owner, &src);
    if (rc == 0) {
        rc = proc_fd_xdev(&src, mnt);
        fd_peek_drop(&src);
    }
    return rc;
}

/* What the checked walk keeps for the ordinary open's replay. */
struct openat2_checked {
    uint32_t seq;
    struct vfs_ref xdev; /* held mount root under NO_XDEV, otherwise NULL */
};

/**
 * The walk of an openat2 path `name` under `resolve` (`vfs_resolve_scoped`),
 * from the directory `dirfd` names: the path it reaches, from the caller's
 * root, into `real`, with the last name it may create after it, and in
 * `checked->seq` the tree's sequence from before the walk began, which the
 * replay compares. `walk` has the walk's
 * VFS_WALK_ flags. A link of /proc to an object at the end of the path,
 * /proc/PID/fd/N or a namespace's, which the system call layer follows
 * itself, is judged here as the walk judges one on the way. Under NO_XDEV
 * keep the mount's root in `checked->xdev`, to put after the replay.
 * Returns 0, or the walk's error.
 */
static int32_t openat2_walk(int dirfd, const char *name, uint32_t resolve, int walk, char *real,
                            struct openat2_checked *checked)
{
    char leaf[VFS_NAME_MAX + 1];
    struct vfs_scope scope;
    struct vfs_ref ref;
    char *anchor = path_buf_get();
    const struct vpath *vp;
    int outside = 0;
    int32_t rc;

    rc = anchor ? openat2_anchor(dirfd, name, resolve, &scope, anchor, &outside) : -ENOMEM;
    if (rc != 0) {
        path_buf_put(anchor);
        return rc;
    }
    rc = vfs_resolve_scoped(&scope, name, walk, &ref, leaf, real);
    checked->seq = scope.seq;
    vfs_ref_release(&scope.anchor);
    path_buf_put(anchor);
    if (rc < 0) {
        return rc;
    }
    vp = rc == 0 && !(walk & VFS_WALK_NOFOLLOW) ? vfs_synth_vpath(ref.mnt, ref.node) : NULL;
    if (rc == 0) {
        rc = openat2_magic(vp, ref.mnt, resolve);
    }
    if (rc >= 0 && (resolve & VFS_RESOLVE_NO_XDEV)) {
        checked->xdev.mnt = ref.mnt;
        checked->xdev.node = ref.mnt->root;
        vfs_ref_hold(&checked->xdev);
        vfs_desc_open(checked->xdev.mnt);
    }
    if (walk & VFS_WALK_LOCK) {
        vfs_walk_unlock(ref.mnt);
    }
    vfs_ref_release(&ref);
    if (rc < 0) {
        return rc;
    }
    return openat2_real(real, (uint32_t)k_strlen(real), rc, leaf, outside);
}

/** Release the mount and root kept across an openat2 replay, if any. */
static void openat2_xdev_put(struct vfs_ref *ref)
{
    if (ref->mnt) {
        vfs_desc_close(ref->mnt, ref->node, NULL);
    }
}

#define OPENAT2_TRIES 4

/**
 * Open `name` from `dirfd` with the checked open flags `flags` and `how`'s
 * mode and RESOLVE_ flags, `end` as `path_names_dir` gives it, using the
 * path page `real`: walk it under its restrictions, then open the path the
 * walk reached as a replay, and walk again while a rename or a mount races
 * the replay, OPENAT2_TRIES times in all. Returns the descriptor or an
 * error.
 */
static long openat2_named(int dirfd, const char *name, const struct open_how *how, int flags,
                          int end, char *real)
{
    uint32_t resolve = (uint32_t)how->resolve;
    int walk = (flags & O_NOFOLLOW) || (flags & (O_CREAT | O_EXCL)) == (O_CREAT | O_EXCL)
                   ? VFS_WALK_NOFOLLOW
                   : 0;
    int tries;
    long rc;

    walk = end ? 0 : walk;
    if ((flags & O_CREAT) && !(flags & O_PATH)) {
        walk |= VFS_WALK_CREATE;
    }
    for (tries = 1;; tries++) {
        struct openat2_checked checked;
        int retry;

        checked.xdev.mnt = NULL;
        checked.xdev.node = NULL;
        rc = openat2_walk(dirfd, name, resolve, walk, real, &checked);
        if (rc != 0) {
            openat2_xdev_put(&checked.xdev);
            return rc;
        }
        if (end == PATH_END_DOT && k_strlen(real) + 3 <= VFS_PATH_MAX) {
            path_put_dot(real, (int)k_strlen(real));
        }
        vfs_replay(!(resolve &
                     (VFS_RESOLVE_NO_SYMLINKS | VFS_RESOLVE_NO_MAGICLINKS | OPENAT2_RESOLVE_ROOT)),
                   checked.seq, checked.xdev.mnt);
        rc = open_named(path_name(real), flags, (uint32_t)how->mode, end);
        retry = vfs_replay_end();
        openat2_xdev_put(&checked.xdev);
        if (!retry || rc != -EAGAIN || tries == OPENAT2_TRIES) {
            break;
        }
    }
    if (rc >= 0) {
        open_done((int)rc, path_name(real), flags);
    }
    return rc;
}

/**
 * openat2(2): dirfd, path, the caller's struct open_how and its size.
 *
 * The arguments are checked as Linux checks them (`open_how_read`,
 * `open_how_flags`) before the path is read. With no RESOLVE_ flag the
 * open is openat(2)'s. With one, the path is walked under its restrictions
 * first (`openat2_walk`), which answer every refusal, and the open then
 * takes the path that walk reached, which has no `..` and no link in it but
 * a link of /proc to an object the walk allowed, as a replay
 * (`vfs_replay`): the replay answers -EAGAIN when it meets a link or when a
 * rename or a mount has changed the tree since the checked walk began, so
 * the open never reaches a file the checked walk did not. The call then
 * walks again, OPENAT2_TRIES times in all, before it gives the caller
 * -EAGAIN, which Linux gives only for a rename racing a scoped `..`.
 */
long sys_openat2(isr_params_t *params)
{
    struct open_how how;
    const char __user *upath = (const char __user *)SC_ARG(params, 1);
    char *name;
    char *real;
    int flags;
    int len;
    long rc;

    if (!current_proc) {
        return -EFAULT;
    }
    rc = open_how_read(SC_ARG(params, 2), (unsigned long)SC_ARG(params, 3), &how);
    if (rc == 0) {
        rc = open_how_flags(&how);
    }
    if (rc < 0) {
        return rc;
    }
    flags = (int)rc;
    if (how.resolve == 0) {
        return open_from_user((int)SC_ARG(params, 0), upath, flags, (uint32_t)how.mode);
    }
    name = path_buf_get();
    real = path_buf_get();
    len = !name || !real ? -ENOMEM : upath ? strncpy_from_user(name, upath, VFS_PATH_MAX) : -EFAULT;
    rc = len < 0 ? len : len == 0 ? -ENOENT : !fd_can_alloc(current_proc->fds) ? -EMFILE : 0;
    if (rc == 0) {
        rc = openat2_named((int)SC_ARG(params, 0), name, &how, flags, path_names_dir(name, len),
                           real);
    }
    path_buf_put(name);
    path_buf_put(real);
    return rc;
}

/**
 * A socket's calls and release are the network stack's, which runs under the
 * big lock; a description that names a file, a device node or a synthetic
 * node is released by a file system; and the description lock that guards
 * an offset (`ops_offset_moves`) is a flag the big lock guards.
 */
int file_ops_unlocked(const fd_entry_t *e)
{
    const struct open_file *ofd;

    if (!e || !e->ops || !(e->ops->flags & FILE_OPS_UNLOCKED) || e->type == FD_TYPE_SOCKET ||
        ops_offset_moves(e)) {
        return 0;
    }
    ofd = e->ofd;
    return !ofd || (!ofd->vmnt && !ofd->dev_mnt && !ofd->synth_mnt);
}

int file_seek_unlocked(const fd_entry_t *e)
{
    return e && e->ops && e->ops->llseek &&
           ((e->ops->flags & FILE_OPS_SEEK_UNLOCKED) || file_ops_unlocked(e));
}

int file_poll_unlocked(const fd_entry_t *e)
{
    return e && e->ops && e->ops->poll &&
           ((e->ops->flags & FILE_OPS_POLL_UNLOCKED) || file_ops_unlocked(e));
}

/**
 * An ioctl touches the held device, not its retained inode or offset.
 * Its FILE_OPS_UNLOCKED dispatcher takes any lock its request needs,
 * as Linux's unlocked_ioctl does even for a file-system device node.
 */
int file_ioctl_unlocked(const fd_entry_t *e)
{
    return e && e->ops && (e->ops->flags & FILE_OPS_UNLOCKED);
}

/**
 * Whether descriptor `e` names a regular file or a directory of a type that
 * sets `reads_unlocked`, as ext2 does, whose reads, listings and attributes
 * then run without the big lock: the type's operations take it where they
 * need it, as Linux reads such a file under its inode's lock alone.
 */
int file_vfs_unlocked(const fd_entry_t *e)
{
    const struct open_file *ofd;

    if (!e || e->ops || (e->type != FD_TYPE_FILE && e->type != FD_TYPE_DIR)) {
        return 0;
    }
    ofd = e->ofd;
    return ofd && ofd->vmnt && !ofd->dev_mnt && !ofd->synth_mnt &&
           vfs_mount_base(ofd->vmnt)->type->reads_unlocked;
}

/** The big lock for a read of `e`: none for a file `file_vfs_unlocked`
 * admits, and `file_ops_lock`'s answer otherwise. */
static int file_read_lock(const fd_entry_t *e)
{
    return file_vfs_unlocked(e) ? 0 : file_ops_lock(e);
}

/**
 * Whether a read or a write of `e`, and nothing else, may run without the
 * big lock: `file_ops_unlocked`'s test but for the mounts of a device node
 * and of a synthetic node, which a read or a write of the object does not
 * touch and the description's end releases under the big lock itself
 * (`ofd_end_big`). /dev/null and /dev/zero opened by path hold one, and are
 * read and written without the lock, as Linux's read_zero and write_null
 * take none (drivers/char/mem.c). It implies `file_ops_unlocked`'s answer
 * but for those mounts, which `open_device_node` may add after the
 * descriptor is published, so that FD_MODE_READ_FREE and
 * FD_MODE_WRITE_FREE (`fd_mode_of`) do not depend on them.
 */
static int file_rw_unlocked(const fd_entry_t *e)
{
    if (!e || !e->ops || !(e->ops->flags & FILE_OPS_UNLOCKED) || e->type == FD_TYPE_SOCKET ||
        ops_offset_moves(e)) {
        return 0;
    }
    return !e->ofd || !e->ofd->vmnt;
}

/**
 * The big lock for a read or a write of `e`, held for the call: none when
 * the FD_MODE_* bit `free`, FD_MODE_READ_FREE or FD_MODE_WRITE_FREE, is set
 * in its mode (`fd_mode`) or there is no descriptor, whose call fails
 * without a lock, and taken otherwise. Returns whether it was taken, for
 * `file_ops_unlock`.
 */
static int rw_call_lock(const fd_entry_t *e, uint32_t free)
{
    if (!e || (fd_mode(e) & free)) {
        return 0;
    }
    bkl_enter();
    return 1;
}

int file_ops_lock(const fd_entry_t *e)
{
    if (file_ops_unlocked(e)) {
        return 0;
    }
    bkl_enter();
    return 1;
}

void file_ops_unlock(int taken)
{
    if (taken) {
        bkl_exit();
    }
}

/**
 * The lookup holds the descriptor for the length of the call in a shared
 * table (`fd_get`), so another sharer's close leaves the slot as it is
 * until the call ends, and the call's own lookups find the same object or,
 * once the close has marked it, none.
 */
int fd_call_lock(int fd)
{
    return file_ops_lock(current_proc ? fd_get(current_proc->fds, fd) : NULL);
}

int file_vfs_writes_unlocked(const fd_entry_t *e)
{
    return file_vfs_unlocked(e) && e->type == FD_TYPE_FILE &&
           vfs_mount_base(e->ofd->vmnt)->type->writes_unlocked;
}

/** The big lock for a truncation, fallocate or sync of descriptor `fd`,
 * held for the call (`fd_get`): none for a file `file_vfs_writes_unlocked`
 * admits, and taken for any other unless the caller holds it, since those
 * calls reach every type's operations. Returns whether it was taken, for
 * `big_give`. */
static int fd_change_lock(int fd)
{
    fd_entry_t *e = current_proc ? fd_get(current_proc->fds, fd) : NULL;

    return file_vfs_writes_unlocked(e) ? 0 : big_take();
}

/**
 * Release descriptor `fd` of `table`, which the caller has marked closing
 * and releasing (`fd_close_claim`, `fd_holds_claim`), with every backing it
 * holds but the record locks, which the close that ends it drops itself,
 * and free its slot. The release may sleep, in a file system's release of
 * the description's last reference among other places: while it does, a
 * lookup, a dup onto the slot, a copy of the table and another release all
 * pass the entry by, and none of them can take a reference to a
 * description that is going.
 *
 * The release runs under the big lock, which the objects of every type not
 * marked unlocked still need; an object marked unlocked is released without
 * it, its last descriptor closing it as Linux's last `fput` releases a
 * file (`fd_release_objects`), and so is a file or directory that
 * `file_vfs_unlocked` admits, whose description's end takes the big lock
 * where it needs it (`fd_ofd_unref`).
 */
static void fd_teardown(fd_table_t *table, int fd)
{
    fd_entry_t *e = fd_slot(table, fd);
    int big = !file_ops_unlocked(e) && !file_vfs_unlocked(e);

    if (big) {
        bkl_enter();
    }
    fd_release_objects(e);
    fd_free_after(table, fd);
    if (big) {
        bkl_exit();
    }
}

/**
 * Close descriptor `fd` of the caller's table. The table's record locks on
 * the file go as soon as the descriptor is claimed, as Linux drops them at
 * every close. Then the entry
 * is released and its slot freed (`fd_teardown`), or, while a call of
 * another thread sharing the table holds it, marked closing, so that the
 * call ends with what it had and the slot is released as that call ends
 * (`fd_holds_release`), which drops no locks: that may be the call of
 * another process sharing the table, or a call of the caller's own that
 * ends after it took a lock again. Linux keeps the file for such a call
 * through the reference `fdget` takes. The choice is made under the table's
 * lock (`fd_close_claim`), so of two closes of one descriptor one answers
 * EBADF. The locks go after the claim, as Linux's close takes the file out
 * of the table before `filp_close` drops its locks (fs/file.c:654-665), so
 * that a lock command of another sharer either finds the descriptor closing
 * after it sets its lock or has set it before the close looks
 * (`fd_lock_still_open`). An epoll_wait holds only its set's descriptor,
 * since a registration polls through its own copy of the descriptor it was
 * made of (io/epoll.c), as Linux's epoll takes no reference to what it
 * watches. Runs without the big lock but where a step needs it. 0, or
 * -EBADF.
 */
static int32_t close_fd(int fd)
{
    fd_table_t *table = current_proc->fds;
    fd_entry_t *e = fd_get(table, fd);
    int rc;

    if (!e) {
        return -EBADF;
    }
    rc = fd_close_claim(table, fd);
    if (rc >= 0) {
        fd_close_locks(e);
    }
    if (rc == 1) {
        fd_teardown(table, fd);
    }
    return rc < 0 ? rc : 0;
}

/**
 * An entry whose release another thread has begun (FD_FLAG_RELEASING) is
 * left to it: two threads whose calls end while one of them sleeps in a
 * release would otherwise both find the entry closing and held by nobody,
 * and release it twice. `fd_holds_claim` lets go of the holds in order,
 * under the table's lock, and marks each slot it gives back for release.
 */
void fd_holds_release(void)
{
    int fd = 0;

    while ((fd = fd_holds_claim(fd)) >= 0) {
        fd_teardown(current_proc->fds, fd);
        fd++;
    }
}

#define CLOSE_RANGE_UNSHARE 0x2u
#define CLOSE_RANGE_CLOEXEC 0x4u

/**
 * close_range takes first, last and flags. Closes each open descriptor from
 * `first` to `last`, or with CLOSE_RANGE_CLOEXEC marks each close-on-exec
 * instead; CLOSE_RANGE_UNSHARE first gives a caller that shares its table a
 * copy of its own. A number past the table is no descriptor. 0, or -EINVAL for
 * a flag it does not take or `first` past `last`, or -ENOMEM.
 */
long sys_close_range(isr_params_t *params)
{
    uint32_t first = (uint32_t)SC_ARG(params, 0);
    uint32_t last = (uint32_t)SC_ARG(params, 1);
    uint32_t flags = (uint32_t)SC_ARG(params, 2);
    uint32_t fd;

    if ((flags & ~(CLOSE_RANGE_UNSHARE | CLOSE_RANGE_CLOEXEC)) != 0 || first > last) {
        return -EINVAL;
    }
    if ((flags & CLOSE_RANGE_UNSHARE) && proc_files_unshare() != 0) {
        return -ENOMEM;
    }
    for (fd = first; fd <= last && fd < (uint32_t)fd_table_cap(current_proc->fds); fd++) {
        fd_entry_t *e = fd_get(current_proc->fds, (int)fd);

        if (e && (flags & CLOSE_RANGE_CLOEXEC)) {
            e->flags |= FD_FLAG_CLOEXEC;
        } else if (e) {
            (void)close_fd((int)fd);
        }
    }
    return 0;
}

/** close(2) takes fd. Returns 0 or -EBADF (`close_fd`). Closing 0, 1 or 2 is
 * legal; a shell or daemon closes one and opens again onto the freed
 * (lowest) number to redirect it. */
long sys_close(isr_params_t *params)
{
    if (!current_proc) {
        return -ESRCH;
    }
    return close_fd((int)SC_ARG(params, 0));
}

/**
 * Flush what the descriptor `e`, which the caller holds and has checked,
 * is open on, by `sync_op`, as `sync_fd` says: a file or directory of a
 * mount through its type, with the pages shared mappings of the file hold
 * written back first, and anything else by writing back the whole cache.
 * The pages are written back under the big lock, taken when the caller
 * lacks it. Returns 0, -EIO, the type's error, a write-back error of the
 * file system recorded since the description's last sync looked, which
 * `sync_op` reports once to each description (`vfs_fsync`), or a failed
 * write-back of the file's mapped pages recorded in the file's own error
 * sequence since the description last looked (`vfs_ino_err_check`), whose
 * cursor moves past it whatever the answer, so that a later pass that wrote
 * the pages does not erase it and the description is not told it twice.
 */
static long sync_entry(fd_entry_t *e,
                       int (*sync_op)(struct vfs_mount *mnt, void *handle, uint32_t *seen))
{
    if (e->ofd && e->ofd->vmnt) {
        int32_t mapped = 0, rc, kept;

        if (filemap_disk_mapped(e->ofd->vmnt, e->ofd->ino)) {
            int big = big_take();

            mapped = filemap_sync_file(e->ofd);
            big_give(big);
        }
        rc = sync_op(e->ofd->vmnt, e->ofd->fhandle, &e->ofd->wb_seen);
        kept = vfs_ino_err_check(e->ofd->vmnt, e->ofd->ino, &e->ofd->ino_seen);
        if (mapped == 0) {
            mapped = rc != 0 ? rc : kept;
        }
        return mapped;
    }
    return pc_sync_wait() != 0 ? -EIO : 0;
}

/**
 * The body of fsync(2) and fdatasync(2), whose argument is fd.
 *
 * Any open descriptor but an O_PATH one is accepted, whatever its type. A
 * descriptor of a file or directory of a mount, the KRFFS root among them,
 * writes back the pages shared mappings of the file hold
 * (`filemap_sync_file`) and goes to the type by `sync_op`. Any other writes
 * back every dirty page of the disk-backed window (`pc_sync_wait`):
 * the whole cache and not one file's pages, which does more than the call
 * asks. When KRFFS is the RAM module the cache is empty, so that flush does
 * nothing and the call only checks the descriptor. Returns 0, -EBADF for a
 * bad descriptor, -EIO when a write-back fails, or the type's error. Dropbear
 * writes its host key with the open/write/fsync/rename idiom and treats an
 * fsync error as fatal, so the error must reach the program.
 *
 * The call holds the big lock unless the descriptor names a file
 * `file_vfs_writes_unlocked` admits, whose type syncs it with no lock of the
 * file's, as Linux's ext2_fsync takes no inode lock (fs/ext2/file.c:153-163
 * in 6.12), and takes the big lock where it still needs it.
 */
static long sync_fd(isr_params_t *params,
                    int (*sync_op)(struct vfs_mount *mnt, void *handle, uint32_t *seen))
{
    fd_entry_t *e;
    long rc;
    int big;

    if (!current_proc) {
        return -ESRCH;
    }
    big = fd_change_lock((int)SC_ARG(params, 0));
    e = fd_get(current_proc->fds, (int)SC_ARG(params, 0));
    rc = !e || (e->flags & FD_FLAG_PATH) ? -EBADF : sync_entry(e, sync_op);
    big_give(big);
    return rc;
}

/** Flush a file and its attributes to stable storage (fsync(2)), by the
 * type's fsync (`vfs_fsync`). Returns as `sync_fd` does. */
long sys_fsync(isr_params_t *params)
{
    return sync_fd(params, vfs_fsync);
}

/** Flush a file's data to stable storage (fdatasync(2)), by the type's
 * fdatasync (`vfs_fdatasync`). Returns as `sync_fd` does. */
long sys_fdatasync(isr_params_t *params)
{
    return sync_fd(params, vfs_fdatasync);
}

/* Busy slots are retried after a yield and a wait with interrupts enabled,
 * so a write in flight can finish and a page dirtied during that write can
 * be flushed. The retries stop after 30 seconds of ticks if slots remain
 * busy, so a stuck writer cannot hold the caller forever. A write failure
 * alone causes no wait. */
int pc_sync_wait(void)
{
    uint32_t start = (uint32_t)kernel_ticks;
    int rc;

    while ((rc = pc_sync()) & PC_SYNC_BUSY) {
        if ((uint32_t)kernel_ticks - start >= MS_TO_TICKS(30000)) {
            break;
        }
        schedule();
        idle_wait_irq_on();
    }
    return rc;
}

/**
 * Flush all buffered file-system writes to stable storage (sync(2)).
 *
 * Takes no argument. The pages shared mappings hold are written back
 * (`filemap_sync_all`), every active mount's `sync` runs and the buffer
 * cache writes its dirty buffers (`vfs_sync_all`), then every dirty page of
 * the disk-backed window goes back via `pc_sync_wait`, which waits for the
 * writes in flight (and does nothing when KRFFS is the RAM module). Like
 * Linux's sync(2), it returns 0 even if a write-back failed; what failed
 * stays dirty for a later call, and a mount whose device failed a write is
 * read-only from then on.
 */
long sys_sync(isr_params_t *params)
{
    (void)params;
    filemap_sync_all();
    vfs_sync_all();
    (void)pc_sync_wait();
    return 0;
}

/**
 * Flush the file system a descriptor is on (syncfs).
 *
 * Its argument is fd. The kernel keeps no list of a mount's own dirty buffers,
 * so this writes back everything, as sync(2) does; GNU `sync -f` and
 * BusyBox's `sync -f` call it. A descriptor of a file of a mount then
 * answers a write-back error its file system recorded since the
 * description's last syncfs looked, once, as Linux's syncfs reports the
 * superblock's `s_wb_err` (fs/sync.c:166, `vfs_syncfs_error`); any other
 * answers 0. -EBADF for a descriptor that is not open or was opened with
 * O_PATH, as on Linux.
 */
long sys_syncfs(isr_params_t *params)
{
    fd_entry_t *e;

    if (!current_proc) {
        return -ESRCH;
    }
    e = fd_get(current_proc->fds, (int)SC_ARG(params, 0));
    if (!e || (e->flags & FD_FLAG_PATH)) {
        return -EBADF;
    }
    (void)sys_sync(params);
    return e->ofd && e->ofd->vmnt ? vfs_syncfs_error(e->ofd->vmnt, &e->ofd->sb_seen) : 0;
}

/* The advice of posix_fadvise(2), as x86 numbers it. */
#define POSIX_FADV_NORMAL_     0u
#define POSIX_FADV_RANDOM_     1u
#define POSIX_FADV_SEQUENTIAL_ 2u
#define POSIX_FADV_WILLNEED_   3u
#define POSIX_FADV_DONTNEED_   4u
#define POSIX_FADV_NOREUSE_    5u

long ksys_readahead(int fd, uint64_t offset, uint64_t count)
{
    fd_entry_t *e;

    if (!current_proc) {
        return -ESRCH;
    }
    e = fd_get(current_proc->fds, fd);
    if (!e || (e->flags & FD_FLAG_PATH) || !(e->flags & FD_FLAG_READ)) {
        return -EBADF;
    }
    if (e->type != FD_TYPE_FILE) {
        return -EINVAL;
    }
    return ksys_fadvise64(fd, (long long)offset, (long long)count, POSIX_FADV_WILLNEED_);
}

/**
 * readahead(2) as x86-64 has it: the descriptor, the offset and the count,
 * one register each. i386's form, which splits the offset, is in
 * arch/i686/sys_ia32.c.
 */
long sys_readahead(isr_params_t *params)
{
    return ksys_readahead((int)SC_ARG(params, 0), (uint64_t)SC_ARG(params, 1),
                          (uint64_t)SC_ARG(params, 2));
}

/**
 * The advice a description keeps sets its readahead (`ofd->ra`), as Linux's
 * `generic_fadvise` sets `f_ra.ra_pages` and FMODE_RANDOM (mm/fadvise.c:
 * 76-98): POSIX_FADV_NORMAL the default window and no RANDOM, RANDOM a read
 * of what is asked alone, and SEQUENTIAL twice the default window and no
 * RANDOM. WILLNEED queues the range's reads and DONTNEED drops its pages
 * from the file page cache (`filecache_willneed`, `filecache_dontneed`);
 * NOREUSE changes nothing, as on Linux, whose flag only its page ageing
 * reads.
 */
long ksys_fadvise64(int fd, long long offset, long long len, uint32_t advice)
{
    fd_entry_t *e;
    struct open_file *ofd;

    if (!current_proc) {
        return -ESRCH;
    }
    e = fd_get(current_proc->fds, fd);
    if (!e || (e->flags & FD_FLAG_PATH)) {
        return -EBADF;
    }
    if (e->type == FD_TYPE_PIPE) {
        return -ESPIPE;
    }
    if (len < 0 || advice > POSIX_FADV_NOREUSE_) {
        return -EINVAL;
    }
    ofd = e->ofd;
    if (!ofd) {
        return 0;
    }
    switch (advice) {
    case POSIX_FADV_NORMAL_:
        ofd->ra.ra_pages = 0;
        ofd->ra.random = 0;
        break;
    case POSIX_FADV_RANDOM_:
        ofd->ra.random = 1;
        break;
    case POSIX_FADV_SEQUENTIAL_:
        ofd->ra.ra_pages = 2u * FILE_RA_PAGES;
        ofd->ra.random = 0;
        break;
    case POSIX_FADV_WILLNEED_:
        filecache_willneed(ofd, (uint64_t)offset, (uint64_t)len);
        break;
    case POSIX_FADV_DONTNEED_:
        filecache_dontneed(ofd, (uint64_t)offset, (uint64_t)len);
        break;
    default:
        break;
    }
    return 0;
}

/**
 * fadvise64(2) as a 64-bit architecture has it: the descriptor, the offset,
 * the length and the advice, one register each. i386's two forms, which
 * split the offset and the length, are in arch/i686/sys_ia32.c.
 */
long sys_fadvise64(isr_params_t *params)
{
    return ksys_fadvise64((int)SC_ARG(params, 0), SC_SIGNED(params, 1), SC_SIGNED(params, 2),
                          (uint32_t)SC_ARG(params, 3));
}

/* The requests of chattr(1) and lsattr(1): _IOR and _IOW of 'f', and _IOR
   of 'v', each of a long, whose size is in the number: 0x80046601 on i686
   and 0x80086601 on x86-64. The argument is an int all the same, as Linux
   reads it. */
#define FS_IOC_LONG       ((uint32_t)sizeof(long) << 16)
#define FS_IOC_GETFLAGS   (0x80006601u | FS_IOC_LONG)
#define FS_IOC_SETFLAGS   (0x40006602u | FS_IOC_LONG)
#define FS_IOC_GETVERSION (0x80007601u | FS_IOC_LONG)

int32_t fs_flags_ioctl(fd_entry_t *entry, uint32_t request, uintptr_t arg, int *handled)
{
    struct vfs_stat st;
    uint32_t want;
    int rc;

    *handled = 0;
    if ((request != FS_IOC_GETFLAGS && request != FS_IOC_SETFLAGS &&
         request != FS_IOC_GETVERSION) ||
        (entry->type != FD_TYPE_FILE && entry->type != FD_TYPE_DIR) || !entry->ofd ||
        !entry->ofd->vmnt || mnt_is_root(entry->ofd->vmnt)) {
        return 0;
    }
    rc = vfs_getattr(entry->ofd->vmnt, entry->ofd->vnode, &st);
    if (rc != 0 || !st.flags_known) {
        return 0;
    }
    *handled = 1;
    if (request != FS_IOC_SETFLAGS) {
        want = request == FS_IOC_GETFLAGS ? st.flags : st.generation;
        return copy_to_user((void __user *)arg, &want, sizeof(want)) ? -EFAULT : 0;
    }
    if (copy_from_user(&want, (const void __user *)arg, sizeof(want)) != 0) {
        return -EFAULT;
    }
    if (!cred_fs_privileged() && proc_fsuid(current_proc) != st.uid) {
        return -EPERM;
    }
    if (((want ^ st.flags) & (VFS_FL_IMMUTABLE | VFS_FL_APPEND)) && !cred_fs_privileged()) {
        return -EPERM;
    }
    st.flags = want;
    return vfs_setattr(entry->ofd->vmnt, entry->ofd->vnode, VFS_SET_FLAGS, &st);
}

/* The requests of cp --reflink and of other programs that share a file's
   extents: _IOW of 0x94, 9, an int, and 13, struct file_clone_range, whose
   first member is the source descriptor as a 64-bit integer. */
#define FICLONE      0x40049409u
#define FICLONERANGE 0x4020940Du

static int32_t copy_regular(fd_entry_t *e);

/** What stands for the file system of what `e` is open on, as Linux's
 * superblock does in the clone checks: a file's or a directory's mount's
 * base, and otherwise its operations, which two pipes share. */
static const void *clone_fs(const fd_entry_t *e)
{
    if ((e->type == FD_TYPE_FILE || e->type == FD_TYPE_DIR) && !e->ops && e->ofd && e->ofd->vmnt) {
        return vfs_mount_base(e->ofd->vmnt);
    }
    return e->ops;
}

int fs_clone_request(uint32_t request)
{
    return request == FICLONE || request == FICLONERANGE;
}

int32_t fs_clone_ioctl(fd_entry_t *entry, uint32_t request, uintptr_t arg, int *handled)
{
    fd_entry_t *src;
    long long srcfd;
    int32_t rc;

    *handled = request == FICLONE || request == FICLONERANGE;
    if (!*handled) {
        return 0;
    }
    if (request == FICLONE) {
        srcfd = (int)arg;
    } else if (copy_from_user(&srcfd, (const void __user *)arg, sizeof(srcfd)) != 0) {
        return -EFAULT;
    }
    src = srcfd < 0 || srcfd > 0x7FFFFFFF ? NULL : fd_get(current_proc->fds, (int)srcfd);
    if (!src) {
        return -EBADF;
    }
    if (clone_fs(src) != clone_fs(entry)) {
        return -EXDEV;
    }
    rc = copy_regular(src);
    if (rc == 0) {
        rc = copy_regular(entry);
    }
    if (rc != 0) {
        return rc;
    }
    if (!(src->flags & FD_FLAG_READ) || !(entry->flags & FD_FLAG_WRITE) ||
        (entry->ofd->status_flags & OFD_APPEND)) {
        return -EBADF;
    }
    return -EOPNOTSUPP;
}

#define XATTR_FOLLOW 0 /* a path, a final symbolic link followed */
#define XATTR_LINK   1 /* a path, the link itself */
#define XATTR_FD     2 /* a descriptor */

/* Private user-copy state for one attribute call. */
struct xattr_args {
    struct vfs_xattr attr;
    char name[256];    /* Linux's full name limit, with its NUL */
    void __user *user; /* the caller's value or list buffer */
};

/** Resolve an attribute path or hold its descriptor's node.
 * Returns 0 with a held ref, 1 for a descriptor outside the VFS, or -errno. */
static int xattr_ref(isr_params_t *params, int how, struct vfs_ref *ref)
{
    fd_entry_t *e;
    char *path;
    int dir, rc;

    if (how == XATTR_FD) {
        e = fd_get(current_proc->fds, (int)SC_ARG(params, 0));
        if (!e || (e->flags & FD_FLAG_PATH))
            return -EBADF;
        if (!e->ofd || !e->ofd->vmnt)
            return 1;
        ref->mnt = e->ofd->vmnt;
        ref->node = e->ofd->vnode;
        rc = vfs_enter(ref->mnt);
        if (!rc)
            vfs_ref_hold(ref);
        return rc;
    }
    rc = path_from_user_dir(AT_FDCWD, (const char __user *)SC_ARG(params, 0), &path, &dir);
    if (rc)
        return rc;
    rc = resolve_named(path_name(path), how == XATTR_LINK ? VFS_WALK_NOFOLLOW : 0, dir, ref);
    path_buf_put(path);
    return rc;
}

/** Copy a call's arguments into kernel memory before taking a file lock.
 * The caller zeroes `a`, sets its operation, and frees attr.value on every
 * return. Empty SET values still have a non-NULL kernel buffer. */
static int xattr_args(isr_params_t *params, struct xattr_args *a)
{
    struct vfs_xattr *arg = &a->attr;
    uintptr_t size;
    int rc;

    if (arg->op != VFS_XATTR_LIST) {
        rc = strncpy_from_user(a->name, (const char __user *)SC_ARG(params, 1), sizeof(a->name));
        if (rc < 0)
            return rc == -ENAMETOOLONG ? -ERANGE : rc;
        if (!rc)
            return -ERANGE;
        arg->name = a->name;
    }
    if (arg->op == VFS_XATTR_SET) {
        arg->flags = (int)SC_ARG(params, 4);
        if (arg->flags & ~3)
            return -EINVAL;
    }
    if (arg->op == VFS_XATTR_REMOVE)
        return 0;
    a->user = (void __user *)(arg->op == VFS_XATTR_LIST ? SC_ARG(params, 1) : SC_ARG(params, 2));
    size = arg->op == VFS_XATTR_LIST ? SC_ARG(params, 2) : SC_ARG(params, 3);
    if (size > 65536 && arg->op == VFS_XATTR_SET)
        return -E2BIG;
    arg->size = size > 65536 ? 65536 : (uint32_t)size;
    arg->value = kmalloc(arg->size ? arg->size : 1);
    if (!arg->value)
        return -ENOMEM;
    if (arg->op == VFS_XATTR_SET && arg->size && copy_from_user(arg->value, a->user, arg->size))
        return -EFAULT;
    return 0;
}

/** Check Linux's user namespace rules under the inode lock.
 * Only regular files and directories take user values; a sticky directory
 * permits its owner or a privileged caller to change them. */
static int xattr_access(const struct vfs_ref *ref, const struct vfs_xattr *arg)
{
    struct vfs_stat st;
    int rc;

    if (arg->op == VFS_XATTR_LIST ||
        !(arg->name[0] == 'u' && arg->name[1] == 's' && arg->name[2] == 'e' &&
          arg->name[3] == 'r' && arg->name[4] == '.'))
        return 0;
    rc = vfs_getattr(ref->mnt, ref->node, &st);
    if (rc)
        return rc;
    if ((st.mode & 0170000u) != 0100000u && (st.mode & 0170000u) != 0040000u)
        return arg->op >= VFS_XATTR_SET ? -EPERM : -ENODATA;
    if (arg->op >= VFS_XATTR_SET && (st.mode & 01000u) && (st.mode & 0170000u) == 0040000u &&
        proc_fsuid(current_proc) != st.uid && !proc_fs_privileged(current_proc))
        return -EPERM;
    return cred_access_ref(ref, arg->op >= VFS_XATTR_SET ? CRED_W : CRED_R, 0);
}

/** Dispatch an attribute call with copies outside the inode lock.
 * Unsupported types retain their empty LIST and -EOPNOTSUPP answers. */
static int32_t xattr_call(isr_params_t *params, int how, int op)
{
    struct vfs_ref ref;
    struct xattr_args a;
    int rc;

    if (!current_proc)
        return -ESRCH;
    rc = xattr_ref(params, how, &ref);
    if (rc)
        return rc == 1 ? (op == VFS_XATTR_LIST ? 0 : -EOPNOTSUPP) : rc;
    if (!ref.mnt->type->ops->xattr) {
        vfs_ref_release(&ref);
        return op == VFS_XATTR_LIST ? 0 : -EOPNOTSUPP;
    }
    k_memset(&a, 0, sizeof(a));
    a.attr.op = op;
    rc = xattr_args(params, &a);
    if (!rc) {
        vfs_node_lock(&ref);
        rc = xattr_access(&ref, &a.attr);
        if (!rc)
            rc = vfs_xattr(ref.mnt, ref.node, &a.attr);
        vfs_node_unlock(&ref);
        if (rc > 0 && a.attr.size && op <= VFS_XATTR_LIST &&
            copy_to_user(a.user, a.attr.value, (uint32_t)rc))
            rc = -EFAULT;
    }
    kfree(a.attr.value);
    vfs_ref_release(&ref);
    return rc;
}

/** Linux's setxattr, dispatched through the VFS attribute operation. */
long sys_setxattr(isr_params_t *params)
{
    return xattr_call(params, XATTR_FOLLOW, VFS_XATTR_SET);
}

/** Linux's lsetxattr, dispatched through the VFS attribute operation. */
long sys_lsetxattr(isr_params_t *params)
{
    return xattr_call(params, XATTR_LINK, VFS_XATTR_SET);
}

/** Linux's fsetxattr, dispatched through the VFS attribute operation. */
long sys_fsetxattr(isr_params_t *params)
{
    return xattr_call(params, XATTR_FD, VFS_XATTR_SET);
}

/** Linux's getxattr, dispatched through the VFS attribute operation. */
long sys_getxattr(isr_params_t *params)
{
    return xattr_call(params, XATTR_FOLLOW, VFS_XATTR_GET);
}

/** Linux's lgetxattr, dispatched through the VFS attribute operation. */
long sys_lgetxattr(isr_params_t *params)
{
    return xattr_call(params, XATTR_LINK, VFS_XATTR_GET);
}

/** Linux's fgetxattr, dispatched through the VFS attribute operation. */
long sys_fgetxattr(isr_params_t *params)
{
    return xattr_call(params, XATTR_FD, VFS_XATTR_GET);
}

/** Linux's listxattr, dispatched through the VFS attribute operation. */
long sys_listxattr(isr_params_t *params)
{
    return xattr_call(params, XATTR_FOLLOW, VFS_XATTR_LIST);
}

/** Linux's llistxattr, dispatched through the VFS attribute operation. */
long sys_llistxattr(isr_params_t *params)
{
    return xattr_call(params, XATTR_LINK, VFS_XATTR_LIST);
}

/** Linux's flistxattr, dispatched through the VFS attribute operation. */
long sys_flistxattr(isr_params_t *params)
{
    return xattr_call(params, XATTR_FD, VFS_XATTR_LIST);
}

/** Linux's removexattr, dispatched through the VFS attribute operation. */
long sys_removexattr(isr_params_t *params)
{
    return xattr_call(params, XATTR_FOLLOW, VFS_XATTR_REMOVE);
}

/** Linux's lremovexattr, dispatched through the VFS attribute operation. */
long sys_lremovexattr(isr_params_t *params)
{
    return xattr_call(params, XATTR_LINK, VFS_XATTR_REMOVE);
}

/** Linux's fremovexattr, dispatched through the VFS attribute operation. */
long sys_fremovexattr(isr_params_t *params)
{
    return xattr_call(params, XATTR_FD, VFS_XATTR_REMOVE);
}

/**
 * Add a seek displacement to a file offset without overflowing.
 *
 * The sum of an offset and a displacement can leave the range of the type
 * for offsets a caller may legally ask about, and signed overflow has no
 * value in C to check afterwards, so the bound is tested first and the
 * arithmetic done unsigned.
 *
 * @return 0, `-EINVAL` when the result would be negative, or
 * `-EOVERFLOW` when it would pass the largest offset a file can have.
 */
static int seek_add(uint64_t base, long long off, long long *out)
{
    const uint64_t max = 0x7FFFFFFFFFFFFFFFull;

    if (base > max) {
        return -EOVERFLOW;
    }
    if (off >= 0) {
        if ((uint64_t)off > max - base) {
            return -EOVERFLOW;
        }
        *out = (long long)(base + (uint64_t)off);
        return 0;
    }
    {
        uint64_t back = (uint64_t)(-(off + 1)) + 1;

        if (back > base) {
            return -EINVAL;
        }
        *out = (long long)(base - back);
    }
    return 0;
}

/** Seek the raw disk window `entry` is open on, under the description's
 * lock, so after any transfer using its offset completes. Returns the new
 * offset, or -ENXIO for a descriptor that is no raw window, -EOVERFLOW for
 * an offset past `limit`, the largest lseek(2) returns, or the error of
 * `raw_seek_resolve`. */
static long raw_lseek(fd_entry_t *entry, long long offset, int whence, uint64_t limit)
{
    uint64_t resolved, raw_size;
    int32_t rc;

    fd_ofd_lock(entry->ofd);
    if (!devfs_fd_raw_size(entry, &raw_size)) {
        fd_ofd_unlock(entry->ofd);
        return -ENXIO;
    }
    rc = raw_seek_resolve(entry->ofd->raw_offset, raw_size, (long long)offset, whence, &resolved);
    if (rc == 0 && resolved > limit) {
        rc = -EOVERFLOW;
    }
    if (rc == 0) {
        entry->ofd->raw_offset = resolved;
    }
    fd_ofd_unlock(entry->ofd);
    return rc != 0 ? rc : (long)resolved;
}

/** Whether `entry` is a directory or a /proc file, which `seek_listing`
 * seeks. */
static int seeks_listing(const fd_entry_t *entry)
{
    return entry->type == FD_TYPE_DIR || entry->type == FD_TYPE_SYNTH_DIR ||
           entry->type == FD_TYPE_PROC;
}

/**
 * Whether `target` is a place in the listing of the directory of a file
 * system that `entry` is open on: 0, the description's own cursor, or a
 * cursor the type's `readdir` gives after one of its entries, which
 * getdents64 reports as that entry's d_off. The listing is read from its
 * start to find it, since a cursor is the type's own and a type need not
 * read from a place it did not give. Returns 0, -EINVAL for any other place,
 * or the error reading the listing gave.
 */
static int dir_cursor_valid(fd_entry_t *entry, uint32_t target)
{
    struct vfs_dirent de;
    uint32_t cursor = 0;
    int got;

    if (target == 0 || target == entry->ofd->offset) {
        return 0;
    }
    while (cursor != target) {
        got = vfs_readdir(entry->ofd->vmnt, entry->ofd->fhandle, &cursor, &de);
        if (got <= 0) {
            return got < 0 ? got : -EINVAL;
        }
    }
    return 0;
}

/**
 * SEEK_DATA and SEEK_HOLE, `whence` 3 and 4, from `off` in a file of `size`
 * bytes, into `*at`, as Linux answers them for a file system that keeps no
 * map of its holes: the whole file is data, and its one hole is its end.
 * SEEK_DATA lands at `off` and SEEK_HOLE at the end; an `off` below 0 or at
 * or past the end is -ENXIO. Returns 0 or -ENXIO.
 */
static int seek_data_hole(uint64_t size, long long off, int whence, long long *at)
{
    if (off < 0 || (uint64_t)off >= size) {
        return -ENXIO;
    }
    *at = whence == 3 ? off : (long long)size;
    return 0;
}

/**
 * SEEK_END, SEEK_DATA or SEEK_HOLE, `whence` 2 through 4, by `off` in the
 * directory `entry` is open on, whose type's cursor is a byte
 * (`dir_seeks_bytes`), from the size its getattr gives, into `*at`. The
 * caller holds the description's lock. Returns 0, or the error of the
 * getattr or of `seek_add` or `seek_data_hole`.
 */
static int dir_seek_size(fd_entry_t *entry, long long off, int whence, long long *at)
{
    struct vfs_stat st;
    int rc = vfs_getattr(entry->ofd->vmnt, entry->ofd->vnode, &st);

    if (rc != 0) {
        return rc;
    }
    return whence == 2 ? seek_add(st.size, off, at) : seek_data_hole(st.size, off, whence, at);
}

/**
 * lseek(2) and _llseek(2) on a directory or a /proc file: the offset
 * `whence` and `off` give, stored in the description under its lock and
 * returned in `*out`, or a negative errno with the offset unchanged. A read
 * or a write of a /proc file holds the same lock (`ops_offset_held`), so a
 * seek waits for one that sleeps.
 *
 * SEEK_SET and SEEK_CUR move the offset; SEEK_END is -EINVAL, as Linux's /proc
 * files and tmpfs directories answer, since neither has a size to seek from,
 * except on a directory of a type whose cursor is a byte (`dir_seek_size`). A
 * /proc file's offset is a byte of its text, the snapshot its open took or the
 * text a read of oom_score_adj makes, through 0xFFFFFFFF, or, for
 * /proc/PID/mem, an address, so it takes any place through the last address,
 * 0xFFFFFFFF on i686. A directory's offset is the cursor of its listing, which
 * getdents64 reports after each entry as the entry's d_off: a directory of /dev
 * or /proc takes the index of any entry through 0x7FFFFFFF; one of ext2,
 * whose cursor is a byte (`dir_seeks_bytes`), any place through the volume's
 * largest file offset (`max_bytes`), as Linux's `generic_file_llseek` gives
 * an ext2 directory through `s_maxbytes`, which its `readdir` moves to the
 * next record that starts there or after; and one of another file system
 * the places `dir_cursor_valid` finds, so rewinddir, and seekdir to a place
 * telldir gave, move it, and no such type's `readdir` is given a place it did
 * not give. Any other place is -EINVAL, and one past `limit`, the largest
 * offset the call can return, -EOVERFLOW. A directory removed by a route
 * that did not look for its descriptors is -EBADF (`fd_node_check`).
 */
static int seek_listing(fd_entry_t *entry, long long off, int whence, uint64_t limit,
                        long long *out)
{
    uint64_t most = entry->type == FD_TYPE_SYNTH_DIR ? 0x7FFFFFFFull : 0xFFFFFFFFull;
    long long at = off;
    int bytes, rc = 0;

    if (procfs_is_mem(entry)) {
        most = (uint64_t)(uintptr_t)-1;
    }

    if (!entry->ofd || (entry->type == FD_TYPE_DIR && !entry->ofd->vmnt)) {
        return -EBADF;
    }
    if (entry->type == FD_TYPE_DIR && fd_node_check(entry) != 0) {
        return -EBADF;
    }
    bytes = entry->type == FD_TYPE_DIR && entry->ofd->vmnt->type->dir_seeks_bytes;
    if (bytes) {
        most = vfs_mount_base(entry->ofd->vmnt)->max_bytes;
    }
    fd_ofd_lock(entry->ofd);
    if (whence == 1) {
        rc = seek_add(entry->ofd->offset, off, &at);
    } else if (bytes && whence >= 2 && whence <= 4) {
        rc = dir_seek_size(entry, off, whence, &at);
    } else if (whence != 0) {
        rc = -EINVAL;
    }
    if (rc == 0 && (at < 0 || (uint64_t)at > most)) {
        rc = -EINVAL;
    }
    if (rc == 0 && entry->type == FD_TYPE_DIR && !bytes) {
        rc = dir_cursor_valid(entry, (uint32_t)at);
    }
    if (rc == 0 && (uint64_t)at > limit) {
        rc = -EOVERFLOW;
    }
    if (rc == 0) {
        entry->ofd->offset = (uint64_t)at;
        *out = at;
    }
    fd_ofd_unlock(entry->ofd);
    return rc;
}

/**
 * lseek(2) and _llseek(2) on `/dev/bootlog`, whose offset counts from the
 * oldest byte the kernel log's ring holds: the offset `whence` and `off`
 * give, returned in `*out` and, when `store` is set, stored in the
 * description, or a negative errno with the offset unchanged. The whole
 * seek holds the description's lock, which the log's read holds across its
 * copy, so a seek through a descriptor that shares the description waits
 * for a read in progress and moves on from where the read left the offset.
 *
 * SEEK_SET, SEEK_CUR and SEEK_END take the sum by `seek_add`, so a
 * displacement cannot overflow it. A place before 0 or past 0xFFFFFFFF, the
 * last the log's offset has, is -EINVAL, a sum past what `seek_add` holds
 * among them, and one past `limit`, the largest offset the call can return,
 * -EOVERFLOW. A caller with nowhere to return the offset asks with `store`
 * clear and fails the call itself.
 */
static int seek_bootlog(fd_entry_t *entry, long long off, int whence, uint64_t limit, int store,
                        long long *out)
{
    uint32_t size = 0;
    long long at = off;
    int rc = 0;

    fd_ofd_lock(entry->ofd);
    (void)devfs_fd_log_size(entry, &size);
    if (whence == 1) {
        rc = seek_add(entry->ofd->offset, off, &at);
    } else if (whence == 2) {
        rc = seek_add((uint64_t)size, off, &at);
    } else if (whence != 0) {
        rc = -EINVAL;
    }
    if (rc == -EOVERFLOW || (rc == 0 && (at < 0 || at > (long long)0xFFFFFFFFu))) {
        rc = -EINVAL;
    }
    if (rc == 0 && (uint64_t)at > limit) {
        rc = -EOVERFLOW;
    }
    if (rc == 0 && store) {
        entry->ofd->offset = (uint64_t)at;
    }
    fd_ofd_unlock(entry->ofd);
    *out = at;
    return rc;
}

/**
 * lseek(2) and _llseek(2) on /fb, whose offset is a byte of the pixels of
 * the mode in force, counted from the top left one: the offset `whence` and
 * `off` give, stored in the description under its lock, which a write(2)
 * holds, and returned in `*out`, or a negative errno with the offset
 * unchanged. SEEK_END counts from the mode's size (`fb_size`), 0 in text
 * mode. A place before 0 is -EINVAL, and one past `limit`, the largest
 * offset the call can return, -EOVERFLOW; a place past the end is kept, as
 * on a file, and a write there is refused.
 */
static int seek_framebuffer(fd_entry_t *entry, long long off, int whence, uint64_t limit,
                            long long *out)
{
    long long at = off;
    int rc = 0;

    fd_ofd_lock(entry->ofd);
    if (whence == 1) {
        rc = seek_add(entry->ofd->offset, off, &at);
    } else if (whence == 2) {
        rc = seek_add((uint64_t)fb_size(), off, &at);
    } else if (whence != 0) {
        rc = -EINVAL;
    }
    if (rc == 0 && at < 0) {
        rc = -EINVAL;
    }
    if (rc == 0 && (uint64_t)at > limit) {
        rc = -EOVERFLOW;
    }
    if (rc == 0) {
        entry->ofd->offset = (uint64_t)at;
        *out = at;
    }
    fd_ofd_unlock(entry->ofd);
    return rc;
}

/** Seek one of devfs's windows, logs or zero-position devices. */
static long device_lseek(fd_entry_t *entry, long long offset, int whence, uint64_t limit)
{
    int seek_rc;
    long long at;
    if (devfs_fd_is_raw(entry)) {
        return raw_lseek(entry, offset, whence, limit);
    }
    if (devfs_fd_log_size(entry, NULL)) {
        seek_rc = seek_bootlog(entry, offset, whence, limit, 1, &at);
        return seek_rc != 0 ? seek_rc : (long)at;
    }
    if (devfs_lseek_zero(entry)) {
        entry->ofd->offset = 0;
        return 0;
    }
    return -ESPIPE;
}

/**
 * lseek(2): move the offset of the descriptor in argument 0 by the offset
 * in argument 1, a signed register's worth, and the whence in argument 2;
 * the new offset, or a negative errno with the offset unchanged.
 *
 * A file of the KRFFS root or of a mounted type takes SEEK_SET, SEEK_CUR
 * and SEEK_END, the last from the size its getattr gives, and SEEK_DATA and
 * SEEK_HOLE (`seek_data_hole`); any other whence, and a place before 0, is
 * -EINVAL. The description's lock is held from the
 * getattr to the store, so a seek through a shared description waits for a
 * transfer that holds it. A raw disk window, /dev/mem and a loop device seek
 * by `raw_lseek`, /dev/bootlog by `seek_bootlog`, /fb by `seek_framebuffer`,
 * and a directory or a /proc file by `seek_listing`. /dev/null, /dev/zero
 * and /dev/full take any seek
 * and land at 0. Any other descriptor, among them a pipe, a socket, a
 * terminal, the mouse and any other device, is -ESPIPE, as POSIX has it for
 * what cannot seek; one opened with O_PATH is -EBADF.
 *
 * The result is a register's worth: on i686 a place at 2 GiB or past it is
 * -EOVERFLOW, which _llseek reaches, and on x86-64 the offset is 64 bits.
 *
 * The call runs without the big lock, which `sys_lseek` takes for what
 * `seek_unlocked` leaves.
 */
static long lseek_call(isr_params_t *params)
{
    int fd = (int)SC_ARG(params, 0);
    long long offset = SC_SIGNED(params, 1);
    uint64_t limit = SC_SIGNED_MAX(params);
    int whence = (int)SC_ARG(params, 2);
    fd_entry_t *entry;
    uint64_t file_size;
    int seek_rc;
    long long at;

    if (!current_proc) {
        return -ESRCH;
    }

    entry = fd_get(current_proc->fds, fd);
    if (!entry || (entry->flags & FD_FLAG_PATH)) {
        return -EBADF;
    }
    if (entry->ops && entry->ops->llseek) {
        at = entry->ops->llseek(entry, offset, whence);
        if (at < 0)
            return (long)at;
        if ((uint64_t)at > limit)
            return -EOVERFLOW;
        entry->ofd->offset = (uint64_t)at;
        return (long)at;
    }
    if (entry->type == FD_TYPE_PIPE) {
        return -ESPIPE; /* a pipe is not seekable (POSIX) */
    }
    if (entry->type == FD_TYPE_DEVICE) {
        return device_lseek(entry, offset, whence, limit);
    }
    if (entry->type == FD_TYPE_CONSOLE || entry->type == FD_TYPE_MOUSE) {
        return -ESPIPE;
    }
    if (entry->type == FD_TYPE_FRAMEBUFFER && entry->ofd) {
        seek_rc = seek_framebuffer(entry, offset, whence, limit, &at);
        return seek_rc != 0 ? seek_rc : (long)at;
    }
    if (seeks_listing(entry)) {
        seek_rc = seek_listing(entry, offset, whence, limit, &at);
        return seek_rc != 0 ? seek_rc : (long)at;
    }
    if (entry->type != FD_TYPE_FILE) {
        return -ESPIPE;
    }

    if (!entry->ofd || !entry->ofd->vmnt) {
        return -EBADF;
    }
    fd_ofd_lock(entry->ofd);
    {
        struct vfs_stat vst;
        int rc = vfs_getattr(entry->ofd->vmnt, entry->ofd->vnode, &vst);

        if (rc != 0) {
            fd_ofd_unlock(entry->ofd);
            return rc;
        }
        file_size = vst.size;
    }

    switch (whence) {
    case 0: /* SEEK_SET */
        at = offset;
        seek_rc = 0;
        break;
    case 1: /* SEEK_CUR */
        seek_rc = seek_add(entry->ofd->offset, offset, &at);
        break;
    case 2: /* SEEK_END */
        seek_rc = seek_add(file_size, offset, &at);
        break;
    case 3: /* SEEK_DATA */
    case 4: /* SEEK_HOLE */
        seek_rc = seek_data_hole(file_size, offset, whence, &at);
        break;
    default:
        at = -1;
        seek_rc = 0;
        break;
    }
    if (seek_rc != 0) {
        fd_ofd_unlock(entry->ofd);
        return (int32_t)seek_rc;
    }

    if (at < 0) {
        fd_ofd_unlock(entry->ofd);
        return -EINVAL;
    }
    if ((uint64_t)at > limit) {
        fd_ofd_unlock(entry->ofd);
        return -EOVERFLOW;
    }

    entry->ofd->offset = (uint64_t)at;
    fd_ofd_unlock(entry->ofd);
    return (long)at;
}

/**
 * Whether a seek of descriptor `e` runs without the big lock, as Linux's
 * llseek takes none but the file's own (`generic_file_llseek_size`,
 * fs/read_write.c:159-185): an object whose `llseek` is marked to run
 * without it (`file_seek_unlocked`), a file or a directory that
 * `file_vfs_unlocked` admits, whose size and listing its type reads under
 * the file's lock and whose offset the description's lock covers, and any
 * descriptor the call refuses without looking at its object, an O_PATH
 * one, a pipe, a socket or a terminal among them. A device, the
 * framebuffer, a /dev or /proc listing and every other file take it.
 */
static int seek_unlocked(const fd_entry_t *e)
{
    if (!e || (e->flags & FD_FLAG_PATH)) {
        return 1;
    }
    if (e->ops && e->ops->llseek) {
        return file_seek_unlocked(e);
    }
    if (e->type == FD_TYPE_FILE || e->type == FD_TYPE_DIR) {
        return file_vfs_unlocked(e);
    }
    return e->type != FD_TYPE_DEVICE && e->type != FD_TYPE_FRAMEBUFFER &&
           e->type != FD_TYPE_SYNTH_DIR && e->type != FD_TYPE_PROC;
}

/** lseek(2): `lseek_call`, under the big lock for a descriptor that
 * `seek_unlocked` does not admit. */
long sys_lseek(isr_params_t *params)
{
    long rc;

    if (!current_proc || bkl_held() ||
        seek_unlocked(fd_get(current_proc->fds, (int)SC_ARG(params, 0)))) {
        return lseek_call(params);
    }
    bkl_enter();
    rc = lseek_call(params);
    bkl_exit();
    return rc;
}

/**
 * The ICMP raw socket stores its protocol and is not one, so the two are told
 * apart here and not by the socket's type.
 */
int sock_is_query(const struct socket *sock)
{
    return sock->type == SOCK_RAW && sock->protocol == 0;
}

/**
 * Read up to `count` bytes into `buf` through the read method of the object
 * behind `entry`, holding the description for the length of the read when
 * the read uses its offset (`ops_offset_held`). Returns the method's
 * answer, -EBADF for a descriptor not open for reading, or -EINVAL for an
 * object with no read method, as Linux's vfs_read tests FMODE_CAN_READ
 * after FMODE_READ, or for an end of `asked` bytes, the request before its
 * cut, past 2^63-1 at the offset the held description has under its lock
 * (`rw_offset_end`), which a sharer's seek may have moved since the
 * caller's check. May sleep.
 */
static int32_t read_ops(fd_entry_t *entry, char __user *buf, uint32_t count, size_t asked)
{
    struct open_file *held = ops_offset_held(entry);
    int32_t rc;

    if (!(entry->flags & FD_FLAG_READ)) {
        return -EBADF;
    }
    if (held) {
        fd_ofd_lock(held);
        if (rw_offset_end(entry, asked) != 0) {
            fd_ofd_unlock(held);
            return -EINVAL;
        }
    }
    rc = entry->ops->read ? entry->ops->read(entry, buf, count) : -EINVAL;
    if (held) {
        fd_ofd_unlock(held);
    }
    return rc;
}

/** Whether a read of the file of `ofd` must take its next page from the
 * pages in memory of the file's shared mappings: whether a shared mapping
 * through a descriptor open for writing has mapped the file
 * (`filemap_disk_stores`), which another thread may have done while the
 * read slept on an earlier page. Without the big lock. */
static int file_read_cut(const struct open_file *ofd)
{
    return filemap_disk_stores(ofd->vmnt, ofd->ino);
}

/**
 * Read up to `count` bytes of the file of `ofd` at `pos` into the user's
 * `buf` through the file page cache (`filecache_file_read`) or the file
 * system, which a caller without the big lock reaches with it, since the
 * type then writes the user's buffer itself. Before each page after the
 * first the read stops when `file_read_cut` says the page must come from a
 * mapping's page in memory; the file system's read of an ext2 file, the one
 * type a shared mapping keeps pages in memory of, goes a page at a time for
 * that. The bytes read, 0 at the end, or the type's error.
 */
static int32_t file_read_plain(struct open_file *ofd, char __user *buf, uint32_t count,
                               uint64_t pos)
{
    uint32_t done = 0, piece;
    int32_t r = 0;
    int big, paged;

    if (filecache_file_read(ofd, buf, count, pos, &r, file_read_cut)) {
        return r;
    }
    big = !bkl_held();
    paged = vfs_mount_base(ofd->vmnt)->type == &vfs_ext2_type;
    if (big) {
        bkl_enter();
    }
    do {
        piece = count - done;
        if (paged && piece > PAGE_SIZE - (uint32_t)((pos + done) % PAGE_SIZE)) {
            piece = PAGE_SIZE - (uint32_t)((pos + done) % PAGE_SIZE);
        }
        if (done > 0 && file_read_cut(ofd)) {
            break;
        }
        r = vfs_read(ofd->vmnt, ofd->fhandle, (char __force *)buf + done, piece, pos + done);
        if (r <= 0) {
            break;
        }
        done += (uint32_t)r;
    } while ((uint32_t)r == piece && done < count);
    if (big) {
        bkl_exit();
    }
    return done > 0 ? (int32_t)done : r;
}

/**
 * Read up to `count` bytes of the file of `ofd` at `pos` into the user's
 * `buf`, with the description's lock held, each page from where its newest
 * bytes are when the read reaches it, as Linux's filemap_read looks each
 * folio up in the file's pages (mm/filemap.c:2649-2700). While no shared
 * mapping through a descriptor open for writing has mapped the file
 * (`file_read_cut`), its pages in memory, if any, equal the file, and
 * `file_read_plain` reads it, asking again before each page, since such a
 * mapping made while the read slept on the disk may have stored into a
 * later page. From the first page where one has, the read holds the big
 * lock, which it takes when the caller did not and records in `*big` for
 * the caller to give back, and goes a page at a time, up to the size the
 * file has then: each page comes from the mappings' page in memory when
 * there is one (`filemap_read_resident`), so that the read sees their
 * stores, those an earlier buffer of the same readv made among them, and
 * any other from `file_read_plain`, whose bytes are the page's when it was
 * chosen, since a mapping that brings the page in while that read sleeps
 * can hold only stores made after it. The bytes read, which stop at the
 * first page that fails after some were read, 0 at the end, or the type's
 * error.
 */
static int32_t file_read_locked(struct open_file *ofd, char __user *buf, uint32_t count,
                                uint64_t pos, int *big)
{
    struct vfs_stat st;
    uint32_t done = 0, piece;
    int32_t r = 0;
    int paged = 0;

    if (count == 0) {
        return file_read_plain(ofd, buf, count, pos);
    }
    while (done < count) {
        if (!paged && file_read_cut(ofd)) {
            if (!bkl_held()) {
                bkl_enter();
                *big = 1;
            }
            r = vfs_getattr(ofd->vmnt, ofd->vnode, &st);
            if (r != 0 || pos + done >= st.size) {
                break;
            }
            if (count - done > st.size - (pos + done)) {
                count = done + (uint32_t)(st.size - (pos + done));
            }
            paged = 1;
        }
        piece = count - done;
        if (paged) {
            if (piece > PAGE_SIZE - (uint32_t)((pos + done) % PAGE_SIZE)) {
                piece = PAGE_SIZE - (uint32_t)((pos + done) % PAGE_SIZE);
            }
            r = filemap_read_resident(ofd->vmnt, ofd->ino, pos + done, buf + done, piece);
            if (r == 0) {
                r = file_read_plain(ofd, buf + done, piece, pos + done);
            }
        } else {
            r = file_read_plain(ofd, buf + done, piece, pos + done);
        }
        if (r <= 0) {
            break;
        }
        done += (uint32_t)r;
        if ((uint32_t)r < piece && (paged || !file_read_cut(ofd))) {
            break;
        }
    }
    return done > 0 ? (int32_t)done : r;
}

/**
 * The answer of a read of the file of `ofd` at `at` whose first byte the
 * caller cannot write, with the description's lock held: 0 when the type
 * has no byte at `at`, -EFAULT when it has one, or the type's error. The
 * type is asked by a read of one byte into a kernel buffer, since what a
 * read returns is not always the size getattr gives: an mqueue file's size
 * is its queue's bytes and its read the status line. Linux's filemap_read
 * returns 0 at the end before its copy, and simple_read_from_buffer, which
 * an mqueue read is (ipc/mqueue.c:655-656 in 6.12), returns 0 at the end
 * of its text and -EFAULT for a copy that fails short of it
 * (fs/libfs.c:1104-1110).
 */
static int32_t file_read_unwritable(struct open_file *ofd, uint64_t at)
{
    char byte;
    int32_t r = vfs_read(ofd->vmnt, ofd->fhandle, &byte, 1, at);

    return r > 0 ? -EFAULT : r;
}

/**
 * Read up to `count` bytes into user buffer `buf` from the file of a mounted
 * type or of the KRFFS root that `entry` is open on: at the description's
 * offset, which moves past what was read, or at `*at` for pread64, which
 * leaves it where it was. The buffer is brought in for writing first and
 * cut before its first page that cannot be written (`iov_prefix`), so that
 * the read ends there with the bytes before it, as Linux's copy stops at
 * its first fault. At the description's offset the end of `asked` bytes,
 * the request before its cut at USER_RW_MAX, is checked again under the
 * description's lock (`rw_offset_end`), since a sharer may have moved the
 * offset while the buffer was faulted in, as Linux checks it under the
 * position lock; `asked` is unused with `at`. Returns the bytes read, 0 at
 * the end of the file, or a negative errno: -EBADF for a descriptor not open
 * for reading or on no file, -EINVAL for that end past 2^63-1, -EFAULT for
 * a first byte the caller cannot write where the file has a byte to read
 * (`file_read_unwritable`), or the type's error. A read of a byte or more
 * reports IN_ACCESS to inotify. The pages mappings of the file hold are
 * read from memory (`file_read_locked`).
 *
 * A regular ext2 file is read through the file page cache
 * (`filecache_file_read`), which keeps each page it reads. Otherwise the
 * file system writes straight into the user buffer, so its pages are
 * range-checked and faulted in first. The description is held across the
 * operation: with dup or fork sharing the offset, a second reader sleeps
 * rather than reading the same bytes while the first is in a request, and a
 * KRFFS relocation cannot move the handle while it is in use.
 *
 * A file `file_vfs_unlocked` admits is read without the big lock, which the
 * read takes from the first page a shared mapping may have stored into the
 * file's pages, to read them (`file_read_locked`), and for inotify's
 * report. A private
 * mapping of the file or a shared one through a descriptor open only for
 * reading, whose pages equal the file, or a mapping of another file does
 * not make the read take it: Linux's filemap_read finds
 * the file's own folios from its inode and takes no lock beyond theirs
 * (mm/filemap.c:2614-2700). Every return after the description's lock
 * leaves by one path, which lets go of both locks, as Linux's ksys_read
 * releases the position on every answer of vfs_read
 * (fs/read_write.c:701-714).
 */
static int32_t file_read(fd_entry_t *entry, char __user *buf, uint32_t count, const uint64_t *at,
                         size_t asked)
{
    struct open_file *ofd = entry->ofd;
    struct kernel_iovec one[1];
    int32_t bytes_read;
    uint32_t room;
    int big = 0;

    if (!(entry->flags & FD_FLAG_READ)) {
        return -EBADF;
    }
    if (!ofd || !ofd->vmnt) {
        return -EBADF;
    }
    one[0].iov_base = (__force uintptr_t)buf;
    one[0].iov_len = count;
    room = iov_prefix(one, 1, 1);
    fd_ofd_lock(ofd);
    if (!at && rw_offset_end(entry, asked) != 0) {
        bytes_read = -EINVAL;
    } else if (room == 0 && count > 0) {
        bytes_read = file_read_unwritable(ofd, at ? *at : ofd->offset);
    } else {
        bytes_read = file_read_locked(ofd, buf, room, at ? *at : ofd->offset, &big);
    }
    if (bytes_read > 0 && !at) {
        ofd->offset += (uint64_t)bytes_read;
    }
    fd_ofd_unlock(ofd);
    if (bytes_read > 0 && inotify_mount_watched(ofd->vmnt)) {
        if (!bkl_held()) {
            bkl_enter();
            big = 1;
        }
        inotify_desc(ofd, IN_ACCESS);
    }
    if (big) {
        bkl_exit();
    }
    return bytes_read;
}

/**
 * Read the socket of the network behind `entry` into the `count` elements
 * of the copied vector `iov`, for read(2) and readv(2), by the socket's
 * type, as Linux's sock_read_iter gives the vector to the socket's recvmsg
 * (net/socket.c:1110-1130 in 6.12): a datagram or raw socket one datagram
 * (`readv_dgram`), anything else its bytes (`stream_readv`), without
 * waiting under O_NONBLOCK, a status flag of the description. The wait, the
 * end of input and -EAGAIN come before the copy, so a buffer that cannot be
 * written, NULL among them, is found by the copy, as Linux's tcp_recvmsg
 * returns 0 at the end of input before it copies (net/ipv4/tcp.c:2683-2692).
 * Returns the bytes read, or -errno: -EOPNOTSUPP for the query socket, and
 * the receive's errors.
 */
static int32_t socket_readv(fd_entry_t *entry, const struct kernel_iovec *iov, int count)
{
    if (sock_is_query(entry->sock)) {
        return -EOPNOTSUPP;
    }
    if (entry->sock->type == SOCK_DGRAM || entry->sock->type == SOCK_RAW) {
        return readv_dgram(entry, iov, count, 0, 0, 0, 0);
    }
    return stream_readv(entry, iov, (uint32_t)count, 0);
}

/** Read up to `count` bytes of the socket of `entry` into the user's `buf`,
 * for `read_fd`: a vector of one (`socket_readv`). */
static int32_t socket_read(fd_entry_t *entry, char __user *buf, uint32_t count)
{
    struct kernel_iovec one[1];

    if (!entry->sock) {
        return -EFAULT;
    }
    one[0].iov_base = (__force uintptr_t)buf;
    one[0].iov_len = count;
    return socket_readv(entry, one, 1);
}

/**
 * Read up to `count` bytes of the console into the user's `buf`, for
 * `read_fd`, whose descriptor `entry` is the console's; -EBADF when it is not
 * open for reading.
 *
 * A console read returns one line, or one burst of raw bytes of at most
 * N_TTY_READ_CHUNK, and the caller loops for more raw bytes. A line longer
 * than N_TTY_READ_CHUNK is copied a chunk at a time (`tty_read_rest`). It
 * waits in the line discipline of `console_tty` until a cooked line or VMIN
 * raw bytes arrive, unless O_NONBLOCK makes it -EAGAIN, and a pending signal
 * ends the wait with -EINTR. While a reader waits, or once a session owns the
 * console, the keyboard interrupt cooks keystrokes into `console_tty` and
 * not into the kernel shell, so the discipline echoes and edits them and
 * turns ^C into SIGINT; `stdin_waiting_proc`, set for the length of the
 * read, keeps the shell's scrollback keys off. A game that owns the
 * framebuffer reads its keys here too, after it has put the keyboard in
 * K_RAW or K_MEDIUMRAW and the terminal in raw mode (kd.h).
 *
 * A read makes no terminal a controlling one, as on Linux, where only an
 * open and TIOCSCTTY do; init's console session claims the console with
 * TIOCSCTTY after setsid, as getty does. A member of a background group is
 * sent SIGTTIN and the read is -EINTR, to be tried again in the foreground,
 * or -EIO when SIGTTIN is blocked or ignored or the group is orphaned
 * (`tty_job_check`), as POSIX has it. The check comes before
 * `stdin_waiting_proc` is set, so no return leaves it set.
 */
static int32_t console_read(fd_entry_t *entry, char __user *buf, uint32_t count)
{
    unsigned char kbuf[N_TTY_READ_CHUNK];
    uint32_t want, done;
    int n;

    if (!(entry->flags & FD_FLAG_READ)) {
        return -EBADF;
    }
    if (fd_tty_hung(entry)) {
        return 0;
    }

    want = count;
    if (want > sizeof(kbuf)) {
        want = sizeof(kbuf);
    }

    n = tty_job_check(&console_tty, SIGTTIN);
    if (n != 0) {
        return n;
    }
    stdin_waiting_proc = current_proc;
    n = tty_read(&console_tty, kbuf, (int)want,
                 entry->ofd && (entry->ofd->status_flags & OFD_NONBLOCK));
    stdin_waiting_proc = NULL;
    if (n < 0) {
        return n;
    }
    done = 0;
    while (n > 0) {
        if (copy_to_user(buf + done, kbuf, (uint32_t)n) != 0) {
            return -EFAULT;
        }
        done += (uint32_t)n;
        want = count - done;
        if (want > sizeof(kbuf)) {
            want = sizeof(kbuf);
        }
        n = tty_read_rest(&console_tty, kbuf, (int)want);
    }
    return (int32_t)done;
}

/**
 * Whether `entry` may be read, as Linux's vfs_read and vfs_readv test
 * FMODE_READ and then FMODE_CAN_READ before anything else: 0, -EBADF for no
 * descriptor, one opened with O_PATH or one not open for reading, or
 * -EINVAL for an object with no read method, an epoll set among them. A
 * directory passes; its read is -EISDIR, as Linux's generic_read_dir.
 */
static int32_t read_allowed(const fd_entry_t *entry)
{
    if (!entry || (entry->flags & FD_FLAG_PATH)) {
        return -EBADF;
    }
    if (entry->type == FD_TYPE_DIR || entry->type == FD_TYPE_SYNTH_DIR) {
        return 0;
    }
    if (!(entry->flags & FD_FLAG_READ)) {
        return -EBADF;
    }
    if (entry->ops && !entry->ops->read) {
        return -EINVAL;
    }
    return 0;
}

/**
 * Read up to `requested` bytes of descriptor `entry`, the caller's lookup,
 * which may be NULL, into the user's `buf`, for read(2) and readv(2); the
 * bytes read, 0 at the end, or a negative errno:
 * `read_allowed`'s, then -EFAULT for the original requested range past the
 * top of user memory, then -EINVAL for a count negative as the kernel's
 * ssize_t or for an end at the description's offset past 2^63-1
 * (`rw_offset_end`), then -EISDIR for a directory, as Linux's vfs_read
 * orders them. A NULL or bad buffer is -EFAULT only once the object's read
 * finds it in its copy, after its end of input, its wait and -EAGAIN, as
 * Linux's vfs_read checks only access_ok, which NULL passes
 * (fs/read_write.c:557 in 6.12): a file of a mount judges it after its end
 * (`file_read`), and an object with a read method, a pipe among them, and a
 * socket in their copies; only the console and the mouse, which have no
 * read method, refuse NULL first. The transfer is capped at
 * USER_RW_MAX after the original range and count have passed. A
 * read of zero bytes returns 0 once those checks have passed, and `buf`
 * may then be NULL, as POSIX allows; from an anonymous descriptor with a
 * read method it goes to that method, since an eventfd and a timerfd read
 * whole eight-byte counts and fail a shorter read, zero bytes among them,
 * with EINVAL, as Linux's do. readv(2) of nothing returns before this, as
 * Linux's does.
 *
 * The read goes by what is behind the descriptor. An object with a vtable,
 * a pipe, a pty, a device or a /proc file among them, reads through it
 * (`read_ops`). A socket's read is `socket_read`'s, the console's
 * `console_read`'s, and that of a file of the KRFFS root or of a mounted
 * type `file_read`'s.
 *
 * A mouse read returns the whole `struct mouse_packet`s waiting that fit in
 * `count`, without waiting, and 0 when `count` is less than one packet. A
 * directory, of a mount or of the synthetic trees, is -EISDIR whatever the
 * count, as Linux's generic_read_dir answers, and a descriptor of any other
 * kind -EBADF.
 */
static int32_t read_entry(fd_entry_t *entry, char __user *buf, size_t requested)
{
    uint32_t count;
    int32_t rc;

    if (!current_proc) {
        return -EFAULT;
    }

    rc = read_allowed(entry);
    if (rc != 0) {
        return rc;
    }
    rc = rw_buffer_range((__force uintptr_t)buf, requested);
    if (rc == 0) {
        rc = rw_offset_end(entry, requested);
    }
    if (rc != 0) {
        return rc;
    }
    count = USER_RW_LEN(requested);
    if (entry->type == FD_TYPE_DIR || entry->type == FD_TYPE_SYNTH_DIR) {
        return -EISDIR;
    }

    if (count == 0) {
        /* An eventfd's or a timerfd's read refuses a count under 8. */
        return entry->type == FD_TYPE_ANON && entry->ops ? read_ops(entry, buf, 0, 0) : 0;
    }
    if (entry->type == FD_TYPE_FILE && !entry->ops) {
        return file_read(entry, buf, count, NULL, requested);
    }
    if (entry->ops) {
        return read_ops(entry, buf, count, requested);
    }
    if (entry->type == FD_TYPE_SOCKET) {
        return socket_read(entry, buf, count);
    }
    if (!buf) {
        return -EFAULT;
    }
    if (entry->type == FD_TYPE_CONSOLE) {
        return console_read(entry, buf, count);
    }

    if (entry->type == FD_TYPE_MOUSE) {
        int packets_requested;
        int packets_read;

        packets_requested = (int)(count / sizeof(struct mouse_packet));
        if (packets_requested <= 0) {
            return 0;
        }

        if (!user_buffer_ok((__force uintptr_t)buf,
                            (uint32_t)packets_requested * sizeof(struct mouse_packet), 1)) {
            return -EFAULT;
        }
        packets_read = mouse_read_packets((struct mouse_packet __force *)buf, packets_requested);

        return packets_read * (int32_t)sizeof(struct mouse_packet);
    }

    return -EBADF;
}

/** `read_entry` for descriptor `fd`, which it looks up. */
static int32_t read_fd(int fd, char __user *buf, size_t requested)
{
    return read_entry(current_proc ? fd_get(current_proc->fds, fd) : NULL, buf, requested);
}

/**
 * read(2): read up to the count in argument 2 of the descriptor in argument
 * 0 into the user's buffer in argument 1 (`read_entry`); the bytes read, 0
 * at the end, or a negative errno. The descriptor and the whole requested
 * range are checked before a count past USER_RW_MAX is cut there, as on
 * Linux. On i686 a count negative as ssize_t is EINVAL after the range
 * check; a compat call on x86-64 has the kernel's 64-bit ssize_t instead.
 * The descriptor is looked up once, as Linux's ksys_read looks it up with
 * fdget_pos, and the call holds the big lock unless its mode says its
 * reads run without it (FD_MODE_READ_FREE, `rw_call_lock`): an object whose
 * type runs without it or a file whose type reads without it.
 */
long sys_read(isr_params_t *params)
{
    fd_entry_t *e = current_proc ? fd_get(current_proc->fds, (int)SC_ARG(params, 0)) : NULL;
    int big = rw_call_lock(e, FD_MODE_READ_FREE);
    long rc = read_entry(e, (char __user *)SC_ARG(params, 1), SC_ARG(params, 2));

    file_ops_unlock(big);
    return rc;
}

/**
 * Read up to `count` bytes of descriptor `fd`, whose entry is `entry`, into
 * `buf`, only as far as that can be done without waiting: the second and
 * later buffers of a readv(2) of a device or a /proc file, whose bytes
 * may arrive over time. Returns the bytes read, 0 when none are waiting or
 * `entry` is NULL, or a negative errno: -EFAULT for a buffer the caller
 * cannot write, or the read's own.
 *
 * Readiness is asked of the object behind the descriptor: its vtable
 * answers `poll`. No other process can take the bytes between the question
 * and the read: kernel code runs under the big lock, the switch an
 * interrupt asks for waits for the system call's exit, and an interrupt
 * only adds bytes. Any other descriptor is read by `read_fd`.
 *
 * A terminal answers 0 whatever it has. Its read returns one line, and a
 * second line in the next buffer would answer a question the reader did not
 * ask.
 */
static int32_t read_ready(fd_entry_t *entry, int fd, char __user *buf, uint32_t count)
{
    if (!entry) {
        return 0;
    }
    if (fd_tty(entry)) {
        return 0;
    }
    if (entry->ops) {
        struct wait_queue *wq = 0;

        if (!entry->ops->poll || !entry->ops->read) {
            return 0;
        }
        if (!(entry->ops->poll(entry, POLLIN, &wq) & POLLIN)) {
            return 0;
        }

        return read_ops(entry, buf, count, count);
    }

    return read_fd(fd, buf, count);
}

/* The datagram bytes `readv_dgram` copies on its stack; a larger copy is
 * made in a block of the heap. */
#define READV_DGRAM_STACK 1024

/**
 * Copy the `n` bytes of a datagram at `message` into the buffers of the
 * copied vector `iov` of `iovcnt` elements, in order: the bytes copied, or
 * -EFAULT when any copy fails, as Linux's copy of a datagram into an
 * iov_iter fails whole (include/net/udp.h:381-385).
 */
static int32_t dgram_spread(const struct kernel_iovec *iov, int iovcnt, const uint8_t *message,
                            uint32_t n)
{
    uint32_t offset = 0;
    int i;

    for (i = 0; i < iovcnt && offset < n; i++) {
        uint32_t chunk = n - offset;

        if (chunk > iov[i].iov_len) {
            chunk = (uint32_t)iov[i].iov_len;
        }
        if (chunk > 0 &&
            copy_to_user((void __user *)iov[i].iov_base, message + offset, chunk) != 0) {
            return -EFAULT;
        }
        offset += chunk;
    }
    return (int32_t)offset;
}

/**
 * The datagram at the head of the queue is copied into a kernel buffer, its
 * sender read and the datagram consumed under one hold of `net_lock`, as net.h
 * requires of a system call, and only then spread across the buffers. Nothing
 * in the hold sleeps, and the timer does not switch a process inside a system
 * call (`sched_preempt`), so no other reader of the socket runs there; the
 * receive interrupt it keeps out appends only at the tail in any case. The
 * spread may sleep, since a user copy backs pages (usercopy.h), and another
 * reader may run then: the datagram is already off the queue, so the two
 * cannot copy one datagram and consume two, and the sender reported is this
 * datagram's. A datagram gone between the wait and the hold counts as one of
 * no bytes. A blocking receive on a socket shut for receiving with nothing
 * queued returns 0 and names no sender, as Linux's udp_recvmsg returns 0
 * with no address there.
 *
 * The buffers' lengths are summed before the hold, and only as much of the
 * datagram as they take is copied: into READV_DGRAM_STACK bytes on the
 * kernel stack, or past that into a block of the heap, without which the
 * call is -ENOMEM with the datagram left queued. The rest of a longer
 * datagram is dropped, and `whole` gets its whole length, so that the caller
 * reports it truncated. A netlink socket notes the sum, before the cut to
 * the datagram, as the size of a read, which sizes its answers
 * (`netlink_note_recv`), as Linux's netlink_recvmsg notes the reader's
 * length (net/netlink/af_netlink.c:1944-1947 in 6.12).
 *
 * The spread runs with interrupts on, through `copy_to_user`, over the
 * vector copied before the wait, as Linux copies the native or compat
 * vector first (lib/iov_iter.c:1408, from fs/read_write.c:1011): a buffer
 * that points at the caller's own iovec array is written as any other, and
 * the destinations after it stay those the call was given. `copy_to_user`
 * fails on an address it cannot fault in, and any copy that fails makes the
 * call -EFAULT, the datagram consumed or, with `peek`, kept, as Linux's
 * udp_recvmsg fails at any element (net/ipv4/udp.c:1932-1949).
 */
int32_t readv_dgram(fd_entry_t *entry, const struct kernel_iovec *iov, int iovcnt,
                    net_addr_t *src_ip, uint16_t *src_port, int peek, int32_t *whole)
{
    uint8_t small[READV_DGRAM_STACK];
    uint8_t *message = small;
    uint32_t want = 0, room;
    int32_t total;
    uint32_t held;
    int nonblock;
    int taken;
    int length;
    int i;

    for (i = 0; i < iovcnt; i++) {
        want += (uint32_t)iov[i].iov_len;
    }
    room = want;

    nonblock = entry->ofd && (entry->ofd->status_flags & OFD_NONBLOCK);
    if (nonblock) {
        taken = socket_dgram_nowait(entry->sock);
        if (taken < 0) {
            return taken;
        }
    } else {
        int rc = socket_dgram_wait(entry->sock);

        if (rc < 0) {
            return rc;
        }
        if (rc == SOCK_RECV_SHUT) {
            if (whole) {
                *whole = 0;
            }
            return 0;
        }
    }

    held = net_lock();
    length = socket_dgram_pending(entry->sock);
    if ((uint32_t)(length < 0 ? 0 : length) < want) {
        want = (uint32_t)(length < 0 ? 0 : length);
    }
    if (want > sizeof(small)) {
        message = kmalloc(want);
        if (!message) {
            net_unlock(held);
            return -ENOMEM;
        }
    }
    taken = socket_dgram_peek(entry->sock, 0, message, (int)want);
    if (taken < 0) {
        taken = 0;
    }
    if (length < taken) {
        length = taken;
    }
    if (entry->sock->family == AF_NETLINK) {
        netlink_note_recv(entry->sock, room);
    }
    if (peek) {
        socket_dgram_sender(entry->sock, src_ip, src_port);
    } else {
        socket_dgram_consume(entry->sock, src_ip, src_port);
    }
    net_unlock(held);
    if (whole) {
        *whole = length;
    }

    total = dgram_spread(iov, iovcnt, message, (uint32_t)taken);
    if (message != small) {
        kfree(message);
    }

    return total;
}

/**
 * The bytes of the buffer `iov` of a readv, at most `left`, that lie in
 * user space and can be written, up to its first page that cannot
 * (`iov_prefix`), which are brought in: 0 when none can.
 */
static uint32_t readv_room(const struct kernel_iovec *iov, uint32_t left)
{
    struct kernel_iovec room[1];

    room[0] = *iov;
    if (room[0].iov_len > left) {
        room[0].iov_len = left;
    }
    if (!user_access_ok(room[0].iov_base, room[0].iov_len)) {
        return 0;
    }
    return iov_prefix(room, 1, 1);
}

/**
 * Read the descriptor `fd`, whose entry is `entry`, into the `count`
 * elements of the copied vector `iov`, `wanted` bytes in all, for `readv`
 * on anything but a file of a mount: the bytes read or a negative errno.
 *
 * How depends on what is behind the descriptor, since a second read is not
 * always reading on. A socket of the network is read as its read(2) reads
 * it (`socket_readv`): a datagram, raw or packet socket once
 * (`readv_dgram`), since one datagram is one message and a second read
 * would take the next one, and any other as a stream. An AF_UNIX socket of
 * any type is one receive into the whole vector (`unix_readv`), as Linux's
 * sock_read_iter gives it to unix_recvmsg: a datagram or sequenced-packet
 * socket takes one message across the elements (net/unix/af_unix.c:2496-2501
 * in 6.12), and a stream socket's copy that faults keeps its message queued
 * (af_unix.c:2921-2927). A stream socket of the network (`stream_readv`)
 * and a pipe (`pipe_readv`) read the vector as one read, as Linux's
 * tcp_recvmsg and pipe_read take an iov_iter: the wait, the end of input
 * and -EAGAIN come first, and a copy that faults leaves its bytes queued.
 *
 * Anything else, a device or a /proc file among them, is read for the first
 * buffer by the ordinary read, which waits if it must, and for each later
 * one only while it has bytes waiting (`read_ready`), since a read of an
 * empty one would wait for input nobody has written; a file is read on into
 * each buffer, since its bytes have no boundary. A terminal is
 * read once: its line discipline returns one line, and a second buffer
 * filled with the next would return two lines where the reader asked for
 * one. Each buffer is brought in for writing before it is read into and cut
 * before its first page that cannot be written (`readv_room`), so that the
 * read ends there with the bytes before it; a first buffer of which no byte
 * can be written goes to the read whole, so that the object answers its
 * wait, end of input and -EAGAIN before the copy fails, as Linux's import
 * checks only that a range lies in user space (lib/iov_iter.c:1474). The
 * walk stops at the first buffer it does not fill, and once a byte has been
 * read, a later failure ends it with the total.
 */
static long readv_elements(fd_entry_t *entry, int fd, const struct kernel_iovec *iov, int count,
                           uint32_t wanted)
{
    uint32_t total = 0;
    int walk;
    int i;

    if (entry->type == FD_TYPE_SOCKET && entry->sock && !entry->ops) {
        return socket_readv(entry, iov, count);
    }
    if (entry->type == FD_TYPE_UNIX) {
        return unix_readv(entry, iov, count, wanted);
    }
    if (pipe_is(entry)) {
        return pipe_readv(entry, iov, count);
    }
    walk = !entry->ops && entry->type == FD_TYPE_FILE;

    for (i = 0; i < count && total < wanted; i++) {
        char __user *buf = (char __user *)iov[i].iov_base;
        uint32_t len;
        int32_t r;

        if (iov[i].iov_len == 0) {
            continue;
        }
        len = readv_room(&iov[i], wanted - total);
        if (len == 0 && total > 0) {
            break;
        }
        if (total > 0 && !walk) {
            r = read_ready(entry, fd, buf, len);
            if (r <= 0) {
                break;
            }
        } else {
            if (len == 0) {
                len = iov[i].iov_len < wanted ? (uint32_t)iov[i].iov_len : wanted;
            }
            r = read_fd(fd, buf, len);
            if (r < 0) {
                return (total > 0) ? (long)total : r;
            }
        }
        total += (uint32_t)r;
        if ((uint32_t)r < iov[i].iov_len) {
            break;
        }
    }

    return (long)total;
}

/**
 * readv(2): read from the descriptor in argument 0 into the buffers of the
 * user's iovec array in argument 1, of the count in argument 2, in order;
 * the total bytes read, or a negative errno. musl's stdio reads through
 * this call whenever it asks for more than one byte, so `fgets`, `fread`
 * and `scanf` depend on it.
 *
 * The descriptor is checked first (`read_allowed`), as Linux's vfs_readv
 * checks it, so a vector that asks for nothing returns 0 only from a
 * descriptor a read could use, a directory among them. A file of a mount is
 * read by `iov_file_read`, which takes the vector whole, as Linux's
 * vfs_iter_read takes an iov_iter, under the description's lock held across
 * the vector and the check of its end at the offset, as Linux holds
 * f_pos_lock. For anything else the vector is copied into the kernel once
 * and checked whole, as Linux's import copies and checks it
 * (`iov_import`): a count past IOV_MAX_LOCAL, or a negative length, is
 * -EINVAL, and an iovec the caller cannot read or a range outside user space
 * is -EFAULT; the total is cut at MAX_RW_COUNT, as Linux's is, and that
 * total ending past 2^63-1 at the description's offset is -EINVAL
 * (`rw_offset_end`); a vector of more than eight elements that the heap has
 * no room for is -ENOMEM. The copy, with its cut lengths, serves the whole
 * transfer (`readv_elements`), so a destination that overwrites a later
 * iovec in the caller's memory does not move that later destination, as
 * Linux copies the vector before any read (lib/iov_iter.c:1408, from
 * fs/read_write.c:1011).
 *
 * The call returns less than was asked whenever a buffer is not filled,
 * which is what a reader must handle; musl's stdio loops.
 */
static long readv(int fd, uintptr_t iov_user, int iovcnt)
{
    struct kernel_iovec local[8], *iov = local;
    fd_entry_t *entry;
    long wanted;

    wanted = read_allowed(current_proc ? fd_get(current_proc->fds, fd) : (fd_entry_t *)0);
    if (wanted != 0) {
        return wanted;
    }
    if (iovcnt < 0 || iovcnt > IOV_MAX_LOCAL) {
        return -EINVAL;
    }
    entry = fd_get(current_proc->fds, fd);
    if (iov_mount_file(entry)) {
        return iov_file_read(entry, iov_user, iovcnt, -1);
    }
    if (iovcnt > (int)(sizeof(local) / sizeof(local[0]))) {
        iov = kmalloc((size_t)iovcnt * sizeof(*iov));
        if (!iov) {
            return -ENOMEM;
        }
    }
    wanted = iov_import(iov, iov_user, iovcnt, -1);
    if (wanted > 0 && rw_offset_end(entry, (size_t)wanted) != 0) {
        wanted = -EINVAL;
    }
    if (wanted > 0) {
        wanted = readv_elements(entry, fd, iov, iovcnt, (uint32_t)wanted);
    }
    if (iov != local) {
        kfree(iov);
    }
    return wanted;
}

/** readv(2): the descriptor, vector and count in the caller's registers,
 * under the big lock unless it is a pipe end or a Unix socket
 * (`vec_call_lock`). */
long sys_readv(isr_params_t *params)
{
    int fd = (int)SC_ARG(params, 0);
    int big = vec_call_lock(fd);
    long rc = readv(fd, SC_ARG(params, 1), (int)SC_ARG(params, 2));

    file_ops_unlock(big);
    return rc;
}

/**
 * dup(2): a copy of the descriptor in argument 0 at the lowest free
 * number; the new number, or -EBADF for a descriptor that is not open,
 * -EMFILE or -ENOMEM (`fd_alloc_from`).
 *
 * The copy shares the open file description, and with it the offset and
 * the status flags, and the object behind it with its vtable; a socket
 * gains a reference. FD_CLOEXEC is clear on the copy and FD_FLAG_PATH is
 * kept, so the copy of an O_PATH descriptor is one too. The fresh
 * description the allocation made is dropped for the shared one.
 */
long sys_dup(isr_params_t *params)
{
    int oldfd = (int)SC_ARG(params, 0);
    fd_entry_t *entry;
    int newfd;

    if (!current_proc) {
        return -ESRCH;
    }
    entry = fd_get(current_proc->fds, oldfd);
    if (!entry) {
        return -EBADF;
    }
    newfd = fd_alloc_from(current_proc->fds, 0);
    if (newfd < 0) {
        return newfd;
    }
    dup_fill(fd_slot(current_proc->fds, newfd), entry, 0);
    fd_install(current_proc->fds, newfd);
    return newfd;
}

/**
 * Take slot `fd` for dup2 and dup3 to put a copy in, closing the
 * descriptor there if there is one, with every backing it held
 * (`fd_teardown`), which may sleep; the slot is reserved under the table's
 * lock (`fd_dup_target`), so no other sharer's open takes it between. 0,
 * or -EBUSY when the slot cannot be had: an open has reserved it and not
 * yet filled it, which is when Linux's dup2 answers EBUSY; or a call of
 * another thread sharing the table holds its descriptor, or the descriptor
 * a close left to such a call, or a close of it is still releasing it,
 * where Linux replaces the descriptor and the call or the close keeps its
 * file.
 */
static int32_t dup_clear(int fd)
{
    fd_table_t *table = current_proc->fds;
    int rc;

    while ((rc = fd_dup_target(table, fd)) == 1) {
        fd_close_locks(fd_slot(table, fd));
        fd_teardown(table, fd);
    }
    return rc == 0 ? 0 : -EBUSY;
}

/**
 * dup2(2) and dup3(2): make the descriptor `newfd` a copy of `oldfd`,
 * closing what `newfd` had open, FD_CLOEXEC set as `cloexec` says;
 * `newfd`, or a negative errno. A bad `oldfd`, and a `newfd` that is
 * negative or past the process's limit, is -EBADF, and when the two are
 * equal the call checks `oldfd` and changes nothing. A `newfd` past the
 * end of the table grows it, and a heap with no room for that is -ENOMEM;
 * a slot `dup_clear` cannot take is -EBUSY.
 *
 * The copy shares the open file description, and with it the offset, as
 * POSIX has it, and the object behind it with its vtable; a socket gains a
 * reference.
 *
 * The replacement of an open `newfd` may sleep (`dup_clear`), and another
 * thread may close `oldfd` meanwhile. The call holds `oldfd`, so the close
 * leaves it to the call, and the copy is made of what the call looked up, as
 * Linux's dup2 installs the file it looked up; the copy is an open
 * descriptor of its own, with none of the source's closing state, and is
 * published whole, with its FD_CLOEXEC.
 */
static long dup_to(int oldfd, int newfd, int cloexec)
{
    fd_entry_t *old_entry;
    int32_t rc;

    if (!current_proc) {
        return -ESRCH;
    }
    old_entry = fd_get(current_proc->fds, oldfd);
    if (!old_entry) {
        return -EBADF;
    }
    if (newfd < 0 || newfd >= fd_limit(current_proc->fds)) {
        return -EBADF;
    }
    if (oldfd == newfd) {
        return newfd;
    }
    rc = fd_table_reach(current_proc->fds, newfd);
    if (rc != 0) {
        return rc;
    }
    if (dup_clear(newfd) != 0) {
        return -EBUSY;
    }
    dup_fill(fd_slot(current_proc->fds, newfd), old_entry, cloexec);
    fd_install(current_proc->fds, newfd);
    return newfd;
}

/** dup2(2): argument 0 copied to argument 1 (`dup_to`). */
long sys_dup2(isr_params_t *params)
{
    return dup_to((int)SC_ARG(params, 0), (int)SC_ARG(params, 1), 0);
}

/** Whether a synthetic node of `kind` is one of the directories, a /sys
 * device link among them, which leads to its device's directory. */
static int synth_kind_is_dir(int kind)
{
    return kind == VNODE_DEV_DIR || kind == VNODE_PTS_DIR || kind == VNODE_PROC_DIR ||
           kind == VNODE_PROC_NET_DIR || kind == VNODE_PROC_PID || kind == VNODE_PROC_FD_DIR ||
           kind == VNODE_PROC_TASK_DIR || kind == VNODE_PROC_SYS_DIR || kind == VNODE_SYS_DIR ||
           kind == VNODE_SYS_LINK || kind == VNODE_PROC_SYSVIPC_DIR ||
           kind == VNODE_PROC_NET_STAT_DIR || kind == VNODE_PROC_NS_DIR ||
           kind == VNODE_PROC_FDINFO_DIR || kind == VNODE_DEV_SUBDIR ||
           kind == VNODE_PROC_IRQ_DIR || kind == VNODE_PROC_ACPI_DIR || kind == VNODE_PROC_FS_DIR;
}

/** The entry root made under /dev (devtree.h) that the descriptor `e` is
 * open on, a directory or an O_PATH descriptor of one, or 0. */
static int synth_fd_made(const fd_entry_t *e)
{
    if (e->type == FD_TYPE_SYNTH_DIR && e->priv &&
        ((const struct synth_dir *)e->priv)->kind == VNODE_DEV_MADE) {
        return ((const struct synth_dir *)e->priv)->pid;
    }
    if (e->type == FD_TYPE_SYNTH_PATH && e->priv &&
        ((const struct vpath *)e->priv)->kind == VNODE_DEV_MADE) {
        return ((const struct vpath *)e->priv)->syskey;
    }
    return 0;
}

/** Whether the synthetic node `vp` is a directory: one of its kind's
 * (`synth_kind_is_dir`), or one root made under /dev that is still there. */
static int synth_vp_is_dir(const struct vpath *vp)
{
    return synth_kind_is_dir(vp->kind) ||
           (vp->kind == VNODE_DEV_MADE && devtree_is_dir(vp->syskey) == 1);
}

/** The parent (devtree.h) that the synthetic directory `parent` holds is to
 * an entry root makes in it: /dev, a directory of its table that is there,
 * or a directory made there. DEVTREE_NONE for any other node, /dev/pts,
 * /proc and /sys among them, and for a mount made read-only. */
static int synth_dev_parent(const struct vfs_ref *parent)
{
    const struct vpath *vp = vfs_synth_vpath(parent->mnt, parent->node);

    if (!vp || (parent->mnt->flags & VFS_MS_RDONLY)) {
        return DEVTREE_NONE;
    }
    if (vp->kind == VNODE_DEV_DIR) {
        return DEVTREE_ROOT;
    }
    if (vp->kind == VNODE_DEV_SUBDIR && devfs_subdir_present(vp->devdir)) {
        return DEVTREE_ROW(vp->devdir);
    }
    if (vp->kind == VNODE_DEV_MADE && devtree_is_dir(vp->syskey) == 1) {
        return vp->syskey;
    }
    return DEVTREE_NONE;
}

/**
 * Make `leaf` in the synthetic directory `parent`, as mkdir(2) and
 * symlink(2) do under /dev: a directory with the permission bits of `mode`
 * when `target` is NULL, else a link to the `target` text, owned by the
 * caller's file-system ids (devtree.h). The caller has refused a name the
 * directory has; a directory of /dev's table that is absent now is -EEXIST
 * too, since its name is the table's. 0, -EROFS for a directory that takes
 * no entry
 * (`synth_dev_parent`), the write permission's error, or `devtree_make`'s.
 */
static int32_t synth_dev_make(const struct vfs_ref *parent, const char *leaf, uint32_t mode,
                              const char *target)
{
    int where = synth_dev_parent(parent);
    int32_t rc;

    if (where == DEVTREE_NONE) {
        return -EROFS;
    }
    if (where == DEVTREE_ROOT && synth_dev_dir_find(leaf, (int)k_strlen(leaf)) >= 0) {
        return -EEXIST; /* a directory of the table, absent now */
    }
    rc = cred_access_ref(parent, CRED_W, 0);
    if (rc == 0) {
        rc = devtree_make(where, leaf, mode, target, proc_fsuid(current_proc),
                          proc_fsgid(current_proc));
    }
    return rc < 0 ? rc : 0;
}

/**
 * The answer of unlink(2) to `leaf/`, `leaf` an entry root made in the
 * synthetic directory `parent`, for which it removes nothing: -EISDIR for a
 * directory, -ENOTDIR for a link, which Linux judges by the name itself, or
 * -EROFS for a name that is no such entry.
 */
static int32_t synth_dev_slash(const struct vfs_ref *parent, const char *leaf)
{
    int where = synth_dev_parent(parent);
    int id = where == DEVTREE_NONE ? 0 : devtree_child(where, leaf, (uint32_t)k_strlen(leaf));

    return !id ? -EROFS : devtree_is_dir(id) == 1 ? -EISDIR : -ENOTDIR;
}

/* The one renameat2 flag this kernel takes: do not replace the new name. */
#define RENAME_NOREPLACE 1
/* Not a renameat2 flag: a slash after either name, for `synth_dev_rename`. */
#define RENAME_SLASH     0x80000000u

static int32_t synth_name_exists(const char *path);

/**
 * Rename `oleaf`, an entry root made in the synthetic directory `opar`, to
 * `nleaf` in `npar`, the last name of `new_path`, both directories that
 * take entries (`synth_dev_parent`), with rename(2)'s rules
 * (`devtree_rename`): RENAME_NOREPLACE in `flags` keeps an entry there, and
 * RENAME_SLASH, a slash after either name, asks for a directory. The caller
 * has checked the write permission of both. 0; -EROFS for a name that is no
 * such entry, a device among them, or a new name that another kind of entry
 * of /dev has; -ENOTDIR for a link under RENAME_SLASH; or `devtree_rename`'s.
 */
static int32_t synth_dev_rename(const struct vfs_ref *opar, const char *oleaf,
                                const struct vfs_ref *npar, const char *nleaf, const char *new_path,
                                uint32_t flags)
{
    int from = synth_dev_parent(opar), to = synth_dev_parent(npar);
    int id = from == DEVTREE_NONE ? 0 : devtree_child(from, oleaf, (uint32_t)k_strlen(oleaf));

    if (!id || to == DEVTREE_NONE ||
        (!devtree_child(to, nleaf, (uint32_t)k_strlen(nleaf)) &&
         synth_name_exists(new_path) == -EEXIST)) {
        return -EROFS;
    }
    if ((flags & RENAME_SLASH) && devtree_is_dir(id) != 1) {
        return -ENOTDIR;
    }
    return devtree_rename(from, oleaf, to, nleaf, (flags & RENAME_NOREPLACE) != 0);
}

/**
 * Remove `leaf`, an entry root made, from the synthetic directory `parent`,
 * as unlink(2) does when `dir` is 0 and rmdir(2) when it is 1. 0; -EROFS for
 * a name that is not such an entry, a device among them; the write
 * permission's error, or `devtree_remove`'s.
 */
static int32_t synth_dev_remove(const struct vfs_ref *parent, const char *leaf, int dir)
{
    int where = synth_dev_parent(parent);
    int32_t rc;

    if (where == DEVTREE_NONE || !devtree_child(where, leaf, (uint32_t)k_strlen(leaf))) {
        return -EROFS;
    }
    rc = cred_access_ref(parent, CRED_W, 0);
    return rc != 0 ? rc : devtree_remove(where, leaf, dir);
}

/**
 * Make `path`, the canonical path of the directory at (`mnt`, `node`), the
 * working directory, and hold the mount it is on: `vfs_cwd_set` takes no hold
 * on /dev or /proc, and a synthetic directory, whose `mnt` is NULL, has
 * none to take. The path and the hold are stored with nothing between them
 * that sleeps, and the old hold is let go after both, so a thread that shares
 * them (CLONE_FS) finds the path and the hold of one directory. Returns 0, or
 * -ENOMEM with nothing changed.
 */
static int32_t cwd_enter(const char *path, struct vfs_mount *mnt, void *node)
{
    int32_t rc = proc_cwd_set(current_proc, path);

    if (rc != 0) {
        return rc;
    }
    vfs_cwd_set(current_proc, mnt, node);
    return 0;
}

/**
 * Enter the directory `ref` holds, a node of a mounted type, for `do_chdir`,
 * `path` being the request as the walk took it. The node must be a directory
 * the caller may search (-ENOTDIR, -EACCES). The working directory is named
 * by the type when it can name it, so a walk through a link records where it
 * arrived and not what was typed, and otherwise by the request with its dots
 * collapsed, which the physical walk that found the node makes exact.
 */
static int32_t chdir_mounted(const struct vfs_ref *ref, char *path)
{
    struct vfs_stat vst;
    char *named;
    int32_t rc = vfs_getattr(ref->mnt, ref->node, &vst);

    if (rc == 0 && (vst.mode & 0170000u) != 0040000u) {
        rc = -ENOTDIR;
    }
    if (rc == 0) {
        rc = (int)cred_access_ref(ref, CRED_X, 0);
    }
    if (rc != 0) {
        return rc;
    }
    path_expand_links(path);
    collapse_dots(path);
    named = ref->mnt->type->ops->path ? path_buf_get() : NULL;
    if (named && vfs_node_path(ref->mnt, ref->node, named, VFS_PATH_MAX) == 0 &&
        path_in_root(named) == 0) {
        rc = cwd_enter(named, ref->mnt, ref->node);
    } else {
        rc = cwd_enter(path, ref->mnt, ref->node);
    }
    path_buf_put(named);
    return rc;
}

/**
 * Write into the path page `path` the canonical path of the synthetic
 * directory `vp`, for `do_chdir`: a /sys directory by its node and key, any
 * other by its kind and its coordinates, with /proc/self resolved to the
 * caller's pid so that a later relative name classifies right. Returns 0;
 * -ENOENT for a /proc directory whose process has exited, a /sys directory
 * whose device is gone or a directory made under /dev that is removed;
 * -ENOTDIR for anything that is no directory; or -EACCES for a /proc/PID/fd
 * the caller may not search by its mode and owner, or a /proc/PID/fdinfo of
 * a process the caller may not trace (`proc_fd_dir_access`).
 */
static int32_t synth_chdir_path(const struct vpath *vp, char *path)
{
    int at = vp->kind == VNODE_PROC_SYS_DIR ? vp->sysent
             : vp->kind == VNODE_DEV_SUBDIR ? vp->devdir
             : vp->kind == VNODE_DEV_MADE   ? vp->syskey
                                            : 0;

    if (synth_dir_of_proc(vp->kind)) {
        if (procfs_resolve_id(vp) < 0) {
            return -ENOENT;
        }
        at = procfs_resolve_pid(vp->pid);
    }
    if (vp->kind == VNODE_DEV_MADE && devtree_is_dir(vp->syskey) < 0) {
        return -ENOENT;
    }
    if (!synth_vp_is_dir(vp)) {
        return -ENOTDIR;
    }
    if (synth_dir_of_proc(vp->kind)) {
        int32_t rc = proc_fd_dir_access(vp->kind, at, CRED_X, NULL);

        if (rc != 0) {
            return rc;
        }
    }
    if (vp->kind == VNODE_SYS_DIR || vp->kind == VNODE_SYS_LINK) {
        if (SYSFS_KEYED(vp->sysent) && !sysfs_device_exists(vp->sysent, vp->syskey)) {
            return -ENOENT;
        }
        synth_sys_path(vp->sysent, vp->syskey, path, VFS_PATH_MAX);
    } else {
        synth_dir_path(vp->kind, at, vp->tid, path, VFS_PATH_MAX);
    }
    return 0;
}

/**
 * Change the working directory to `path`, an absolute canonical path in a
 * path page, which this may write over.
 *
 * The working directory keeps a canonical path from the process's root
 * (`path_in_root`), so each kind of directory is named its own way: a
 * synthetic one by its kind, with /proc/self resolved to the caller's pid so
 * that a later relative name classifies right; one of another type as
 * `chdir_mounted` names it; one of the KRFFS boot root by its node. Entering
 * a directory takes search permission on it; anything else is -ENOTDIR. The
 * KRFFS root is a directory whatever its stored mode says, since no `mkdir`
 * made it. "/" is taken without a walk only when it names the KRFFS boot
 * root itself, which everyone may search; a root chroot gave the process is
 * resolved and checked like any other directory.
 *
 * A type that keeps a node only while it is open, mounted KRFFS, counts no
 * hold for the walk's reference, and the checks between the walk and the
 * record may sleep for the disk. The node is pinned (`pin`) under the lock
 * the walk ends with, and the pin is let go once the working directory
 * records the node, so an rmdir that comes between finds the pin, and one
 * that comes after finds the working directory, and neither frees the
 * directory the record then names.
 *
 * Returns 0, or a negative errno: the walk's, -ENOTDIR, -EACCES, -ENOENT
 * for a /proc directory whose process has exited or a /sys directory whose
 * device is gone, -ENAMETOOLONG, or -ENOMEM.
 */
static int32_t do_chdir(char *path)
{
    struct vfs_ref ref;
    const struct vpath *svp;
    krffs_node_t *node;
    void *pin = NULL;
    int32_t rc;

    if (names_tree_root(path) && vfs_root_mount()->type == &vfs_krffs_type) {
        return cwd_enter("/", NULL, NULL);
    }
    rc = vfs_resolve(0, path, VFS_WALK_LOCK, &ref, 0, 0);
    if (rc != 0) {
        return rc;
    }
    if (!mnt_is_root(ref.mnt) && ref.mnt->type->ops->pin) {
        rc = ref.mnt->type->ops->pin(ref.mnt, ref.node, &pin);
    }
    vfs_walk_unlock(ref.mnt);
    if (rc != 0) {
        vfs_ref_release(&ref);
        return rc;
    }
    svp = vfs_synth_vpath(ref.mnt, ref.node);
    if (svp) {
        struct vpath vp = *svp;

        vfs_ref_release(&ref);
        rc = synth_chdir_path(&vp, path);
        return rc != 0 ? rc : cwd_enter(path, NULL, NULL);
    }
    node = ref.node;
    if (!mnt_is_root(ref.mnt)) {
        rc = chdir_mounted(&ref, path);
        if (pin) {
            ref.mnt->type->ops->unpin(ref.mnt, pin);
        }
        vfs_ref_release(&ref);
        return rc;
    }
    if (node != krffs_fs->node && !KRFFS_IS_DIR(node->mode)) {
        vfs_ref_release(&ref);
        return -ENOTDIR;
    }
    rc = cred_access_node(node, CRED_X, 0);
    if (rc == 0 && krffs_node_path(node, path, VFS_PATH_MAX) != 0) {
        rc = -ENAMETOOLONG;
    }
    if (rc == 0) {
        rc = path_in_root(path);
    }
    if (rc == 0) {
        rc = cwd_enter(path, ref.mnt, node);
    }
    vfs_ref_release(&ref);
    return rc;
}

/**
 * chdir(2): make the directory the user's path in argument 0 names the
 * working directory; 0 or a negative errno (`do_chdir`). The working
 * directory records the canonical path of the directory the walk reached,
 * of the KRFFS root, of a mounted type, or of /dev, /proc or /sys. A NULL
 * path is -EFAULT, a path to anything but a directory -ENOTDIR, and a
 * missing one -ENOENT.
 */
long sys_chdir(isr_params_t *params)
{
    const char __user *upath = (const char __user *)SC_ARG(params, 0);
    char *path;
    int32_t rc;

    if (!current_proc || !upath) {
        return -EFAULT;
    }
    rc = path_from_user(AT_FDCWD, upath, &path);
    if (rc != 0) {
        return rc;
    }
    rc = do_chdir(path);
    path_buf_put(path);
    return rc;
}

/**
 * fchdir(2): make the directory the descriptor in argument 0 is open on the
 * working directory; 0 or a negative errno, -EBADF for a descriptor that is
 * not open.
 *
 * The working directory's path is the one the descriptor's `*at` calls
 * resolve against (`fd_dir_path`), so it follows a directory a type can name
 * after a rename; a descriptor on anything but a directory is -ENOTDIR, and
 * one on a directory ext2 or tmpfs removed since is -ENOENT. A synthetic
 * directory is named by its kind, as `do_chdir` names it, in whatever root
 * the caller has, since a root chroot gave it has /dev, /proc or /sys by a
 * bind, which the path from the root of the mount table does not pass
 * through. Entering takes search permission on the directory and the mount
 * hold chdir takes. A synthetic directory grants search to everybody but
 * /proc/PID/fd and fdinfo (`proc_fd_dir_access`): the fd directory of a
 * process that has ended is judged by its owner at the open, and the fdinfo
 * directory of one is -ESRCH. A
 * descriptor opened with O_PATH does as well as any other. A directory
 * outside the process's root, on a descriptor opened before a chroot or a
 * pivot_root, is entered as on Linux: the working directory keeps its path
 * from the root of the mount table (`cwd_outside`), relative names are
 * resolved from it, and getcwd answers -ENOENT until a chdir.
 */
long sys_fchdir(isr_params_t *params)
{
    fd_entry_t *entry;
    struct vfs_ref ref;
    char *path;
    int outside = 0;
    int32_t rc;

    if (!current_proc) {
        return -ESRCH;
    }
    entry = fd_get(current_proc->fds, (int)SC_ARG(params, 0));
    if (!entry) {
        return -EBADF;
    }
    path = path_buf_get();
    if (!path) {
        return -ENOMEM;
    }
    if (entry->type == FD_TYPE_SYNTH_DIR) {
        const struct synth_dir *sd = (const struct synth_dir *)entry->priv;

        rc = sd ? 0 : -EBADF;
        if (rc == 0) {
            rc = proc_fd_dir_access(sd->kind, procfs_resolve_pid(sd->pid), CRED_X, sd);
        }
        if (rc == 0) {
            synth_dir_path(sd->kind, sd->pid, sd->tid, path, VFS_PATH_MAX);
            rc = cwd_enter(path, NULL, NULL);
        }
        path_buf_put(path);
        return rc;
    }
    rc = fd_dir_path(entry, path, VFS_PATH_MAX, &outside);
    if (rc == 0 && outside) {
        rc = k_strlen(path) + 2 > VFS_PATH_MAX ? -ENAMETOOLONG : 0;
        if (rc == 0) {
            k_memmove(path + 1, path, k_strlen(path) + 1);
        }
    }
    if (rc == 0 && (!entry->ofd || !entry->ofd->vmnt)) {
        rc = -EBADF;
    } else if (rc == 0) {
        ref.mnt = entry->ofd->vmnt;
        ref.node = entry->ofd->vnode;
        rc = mnt_is_root(ref.mnt) ? cred_access_node((krffs_node_t *)ref.node, CRED_X, 0)
                                  : cred_access_ref(&ref, CRED_X, 0);
        if (rc == 0) {
            rc = cwd_enter(path, ref.mnt, ref.node);
        }
    }
    path_buf_put(path);
    return rc;
}

/**
 * Whether the node `ref` holds may be a process's root: a directory, which
 * the KRFFS root is whatever its stored mode says, that the caller may
 * search, and a caller with CAP_SYS_CHROOT, checked in Linux's order.
 * A directory of /dev or /proc is EINVAL: those types take the rest
 * of a path in one call, and the resolver could not stop `..` inside it.
 * The exception is a /proc/PID/fdinfo the caller may read, which a
 * browser's sandbox enters to have a root that holds nothing once that
 * process has ended (`sys_chroot`). Returns 0, -ENOTDIR, -EACCES, -EPERM
 * or -EINVAL.
 */
static int32_t root_check(const struct vfs_ref *ref)
{
    const struct vpath *vp = vfs_synth_vpath(ref->mnt, ref->node);
    const krffs_node_t *node = ref->node;
    struct vfs_stat vst;
    int32_t rc = 0;

    if (vp) {
        rc = synth_vp_is_dir(vp) ? 0 : -ENOTDIR;
    } else if (mnt_is_root(ref->mnt)) {
        rc = (node == krffs_fs->node || KRFFS_IS_DIR(node->mode)) ? 0 : -ENOTDIR;
        if (rc == 0) {
            rc = cred_access_node(node, CRED_X, 0);
        }
    } else {
        rc = vfs_getattr(ref->mnt, ref->node, &vst);
        if (rc == 0 && (vst.mode & S_IFMT) != S_IFDIR) {
            rc = -ENOTDIR;
        }
        if (rc == 0) {
            rc = cred_access_ref(ref, CRED_X, 0);
        }
    }
    if (rc == 0 && vp && vp->kind == VNODE_PROC_FDINFO_DIR &&
        (procfs_resolve_id(vp) < 0 || !procfs_may_read(procfs_resolve_pid(vp->pid)))) {
        rc = -EACCES;
    }
    if (rc == 0 && !cap_holds(current_proc, CAP_SYS_CHROOT)) {
        rc = -EPERM;
    }
    return rc == 0 && vp && vp->kind != VNODE_PROC_FDINFO_DIR ? -EINVAL : rc;
}

/**
 * The working directory's path from the root of the mount table, into `buf`
 * of VFS_PATH_MAX bytes: its path from the calling process's root, behind
 * the path of that root when chroot gave the process one, or the path an
 * outside one keeps (`cwd_outside`).
 *
 * The path and the root are read together before anything sleeps, and the
 * path is copied into `buf` then: the threads that share them (CLONE_FS) may
 * change either while the root's path is rebuilt from its volume, and a
 * chdir frees a long path's string. The root is held while its path is
 * rebuilt. Returns 0, -ENOMEM, -EBUSY for a root on a mount being unmounted,
 * the error `vfs_node_path` gives, or -ENAMETOOLONG for a path that does not
 * fit.
 */
static int32_t cwd_global(char *buf)
{
    uint32_t cl = (uint32_t)k_strlen(proc_cwd(current_proc));
    struct vfs_ref root;
    struct vfs_mount *mnt;
    char *rpath;
    uint32_t rl;
    int32_t rc;

    k_memcpy(buf, proc_cwd(current_proc), cl + 1);
    if (cwd_outside(buf)) {
        k_memmove(buf, buf + 1, cl);
        return 0;
    }
    root.mnt = current_proc->fs->root_mnt;
    root.node = current_proc->fs->root_node;
    if (!root.mnt) {
        return 0;
    }
    rpath = path_buf_get();
    if (!rpath) {
        return -ENOMEM;
    }
    mnt = root.mnt;
    if (vfs_enter(mnt) != 0) {
        path_buf_put(rpath);
        return -EBUSY;
    }
    vfs_ref_hold(&root);
    rc = vfs_node_path(mnt, root.node, rpath, VFS_PATH_MAX);
    vfs_ref_release(&root);
    if (rc == 0) {
        rl = (uint32_t)k_strlen(rpath);
        if (cl == 1) {
            k_memcpy(buf, rpath, rl + 1);
        } else if (rl + cl + 1 > VFS_PATH_MAX) {
            rc = -ENAMETOOLONG;
        } else {
            k_memmove(buf + rl, buf, cl + 1);
            k_memcpy(buf, rpath, rl);
        }
    }
    path_buf_put(rpath);
    return rc;
}

/**
 * Make the directory `ref` holds the calling process's root, and keep the
 * working directory in it: `buf`, a path page, takes the working
 * directory's path, which stays where it is under its path from the new
 * root when it is inside it, and otherwise moves to the new root.
 *
 * The working directory's new path is made before anything changes, and is
 * stored just before the root, with nothing between the two that sleeps: a
 * thread that shares both (CLONE_FS) never joins a path from one root to
 * the other. A working directory that moves takes its hold on the new root
 * last, after the old root is let go. Returns 0: a working directory whose
 * new path cannot be made, for want of a page or of room, moves to the new
 * root as well.
 */
static int32_t root_enter(const struct vfs_ref *ref, char *buf)
{
    char *root = path_buf_get();
    int32_t rc = root ? cwd_global(buf) : -ENOMEM;

    if (rc == 0) {
        rc = vfs_node_path(ref->mnt, ref->node, root, VFS_PATH_MAX);
    }
    if (rc == 0) {
        rc = path_strip(buf, root);
    }
    path_buf_put(root);
    if (rc == 0) {
        rc = proc_cwd_set(current_proc, buf);
    }
    if (rc != 0) {
        (void)proc_cwd_set(current_proc, "/");
    }
    vfs_root_set(current_proc, ref->mnt, ref->node);
    if (rc != 0) {
        vfs_cwd_set(current_proc, ref->mnt, ref->node);
    }
    return 0;
}

/**
 * chroot(2) takes path. Make the directory `path` names the calling process's
 * root: its absolute paths and the absolute targets of its symbolic links start
 * there, and `..` there stays there (fs/vfs.Readme.md).
 *
 * The refusals come in Linux's order: the path's own (ENOENT, ENOTDIR,
 * ELOOP, ENAMETOOLONG, EACCES on the way), then `root_check`'s. The working
 * directory stays where it is when it is inside the new root, and moves to
 * the new root when it is outside, since it is a path from the process's
 * root; Linux leaves it where it was either way. fork passes the root on
 * and exec keeps it. While a process has it, rmdir refuses the directory
 * with EBUSY and the root holds its mount against unmount.
 *
 * A /proc/PID/fdinfo becomes the root of the tmpfs of memfd_create, an
 * empty directory no name is ever made in and no path reaches. Linux's root
 * is the directory itself, which lists that process's descriptors while it
 * lives and nothing after; the sandbox that does this has a helper process
 * enter it, through CLONE_FS, and end.
 */
long sys_chroot(isr_params_t *params)
{
    const char __user *upath = (const char __user *)SC_ARG(params, 0);
    struct vfs_ref ref;
    char *path;
    int32_t rc;

    if (!current_proc || !upath) {
        return -EFAULT;
    }
    rc = path_from_user(AT_FDCWD, upath, &path);
    if (rc != 0) {
        return rc;
    }
    rc = vfs_resolve(0, path, 0, &ref, 0, 0);
    if (rc == 0) {
        rc = root_check(&ref);
        if (rc == 0 && vfs_synth_vpath(ref.mnt, ref.node)) {
            struct vfs_ref empty;

            empty.mnt = vfs_memfd_mount();
            empty.node = empty.mnt->root;
            rc = root_enter(&empty, path);
        } else if (rc == 0) {
            rc = root_enter(&ref, path);
        }
        vfs_ref_release(&ref);
    }
    path_buf_put(path);
    return rc;
}

/**
 * getcwd(2): copy the working directory's path from the process's root,
 * with its NUL, into the user's buffer in argument 0, of the size in
 * argument 1; the path's length with its NUL, as Linux's system call
 * returns, or -EFAULT for a NULL or bad buffer, -EINVAL for a size of 0,
 * -ERANGE for a path that does not fit, -ENOENT for a working directory
 * outside the root, which no path from the root names, as the C libraries
 * report Linux's "(unreachable)" path, or -ENOMEM when no path page is to
 * be had.
 *
 * The path is copied into a path page before the copy out, which may sleep
 * on a fault: a thread that shares the working directory (CLONE_FS) may
 * chdir meanwhile, which frees a long path's string.
 */
long sys_getcwd(isr_params_t *params)
{
    char __user *buf = (char __user *)SC_ARG(params, 0);
    uint32_t size = USER_LEN32(SC_ARG(params, 1));
    uint32_t cwd_len;
    char *cwd;
    int32_t rc;

    if (!current_proc) {
        return -ESRCH;
    }
    if (!buf) {
        return -EFAULT;
    }
    if (size == 0) {
        return -EINVAL;
    }
    cwd = path_buf_get();
    if (!cwd) {
        return -ENOMEM;
    }
    cwd_len = (uint32_t)k_strlen(proc_cwd(current_proc));
    k_memcpy(cwd, proc_cwd(current_proc), cwd_len + 1);
    if (cwd_outside(cwd)) {
        rc = -ENOENT;
    } else if (cwd_len + 1 > size) {
        rc = -ERANGE;
    } else if (copy_to_user(buf, cwd, cwd_len + 1) != 0) {
        rc = -EFAULT;
    } else {
        rc = (int32_t)(cwd_len + 1);
    }
    path_buf_put(cwd);
    return rc;
}

/*
 * The attributes of a file as the stat calls gather them, before
 * `stat64_put` lays them out in the user's stat64 or `statx_from_kstat` in a
 * struct statx. The times are 64-bit seconds on every architecture: i386's
 * stat64 keeps the low 32 bits of each, and statx reports them whole.
 */
struct kstat {
    uint64_t st_dev;
    uint64_t st_ino;
    uint32_t st_mode;
    uint32_t st_nlink;
    uint32_t st_uid;
    uint32_t st_gid;
    uint64_t st_rdev;
    int64_t st_size;
    uint32_t st_blksize;
    uint64_t st_blocks;
    int64_t st_atime;
    uint32_t st_atime_nsec;
    int64_t st_mtime;
    uint32_t st_mtime_nsec;
    int64_t st_ctime;
    uint32_t st_ctime_nsec;
    /* The id of the mount the file is on, as /proc/PID/mountinfo gives it,
     * which statx reports; 0 for an object on no mount of the table. */
    uint32_t st_mnt_id;
};

/**
 * Report the three times a KRFFS node stores into a kstat. No read
 * advances the access time (`krffs_stamp_at` in fs/krffs.c): the file
 * system is behind a write-back page cache, and stamping an access time at
 * every read would dirty a page at every read. One set by utimensat is
 * reported as it was set.
 */
static void stat64_node_times(struct kstat *kst, const krffs_node_t *node)
{
    kst->st_atime = node->access_time.seconds;
    kst->st_atime_nsec = (uint32_t)node->access_time.nanoseconds;
    kst->st_mtime = node->modification_time.seconds;
    kst->st_mtime_nsec = (uint32_t)node->modification_time.nanoseconds;
    kst->st_ctime = node->attribute_change_time.seconds;
    kst->st_ctime_nsec = (uint32_t)node->attribute_change_time.nanoseconds;
}

/**
 * Report the boot time into a kstat, for a node with no history of its own: a
 * device, a /proc file, a pipe, a socket. It is `btime` of /proc/stat, and not
 * the time now, since a synthetic file whose modification time moved at every
 * stat would look freshly written to a program comparing times, as make does;
 * it moves only when the clock is set.
 */
static void stat64_boot_times(struct kstat *kst)
{
    int64_t boot = rtc_boot_epoch();

    kst->st_atime = boot;
    kst->st_mtime = boot;
    kst->st_ctime = boot;
    kst->st_atime_nsec = 0;
    kst->st_mtime_nsec = 0;
    kst->st_ctime_nsec = 0;
}

/** Give `kst` the identity of a read-only /proc file that is not a
 * process's: proc's device, a regular file of mode 0444, and the inode
 * `inode`. Returns 0, for `synth_stat` to return. */
static int32_t synth_proc_stat(struct kstat *kst, uint32_t inode)
{
    kst->st_dev = PROCFS_FSID;
    kst->st_mode = S_IFREG | 0444;
    kst->st_ino = inode;
    return 0;
}

/**
 * The identity of a node of /sys, `follow` taking a device's link for the
 * directory it leads to, whether or not its device is still there: what a
 * descriptor open on the node reports after the device has gone. A uevent's
 * size is 4096, as Linux gives every attribute file.
 */
static void synth_sys_identity(const struct vpath *vp, struct kstat *kst, int follow)
{
    kst->st_dev = SYSFS_FSID;
    if (vp->kind == VNODE_SYS_FILE) {
        kst->st_mode = S_IFREG | (vp->sysent == SYSFS_REG ? sysfs_node_mode(vp->syskey)
                                  : sysfs_attr_writable(vp->sysent, vp->procfile) ? 0644u
                                  : sysfs_attr_private(vp->sysent, vp->procfile)  ? 0400u
                                                                                  : 0444u);
        kst->st_size = 4096;
        if (vp->sysent == SYSFS_PCI_DEVICE && vp->procfile >= SYSFS_PCI_RESOURCE0) {
            /* Both names of a BAR's file are root's and as long as the BAR. */
            kst->st_mode = S_IFREG | 0600u;
            kst->st_size = (int64_t)sysfs_pci_bar_size(
                vp->syskey, (vp->procfile - SYSFS_PCI_RESOURCE0) % SYSFS_PCI_BARS);
        }
        kst->st_ino = sysfs_ino(vp->sysent, vp->syskey, 1 + vp->procfile);
    } else if (vp->kind == VNODE_SYS_LINK && !follow && vp->sysreg) {
        kst->st_mode = 0120777; /* S_IFLNK | 0777 */
        kst->st_ino = sysfs_ino(SYSFS_REG, vp->sysreg, 0);
    } else if (vp->kind == VNODE_SYS_LINK && !follow) {
        kst->st_mode = 0120777; /* S_IFLNK | 0777 */
        kst->st_ino =
            sysfs_ino(vp->sysent, vp->syskey,
                      SYSFS_IS_BLOCK(vp->sysent) || vp->sysent == SYSFS_RFKILL ? 0xFF : 2);
    } else {
        kst->st_mode = S_IFDIR | 0555;
        kst->st_ino = sysfs_ino(vp->sysent, vp->syskey, 0);
    }
}

/** The identity of a node of /sys, as `synth_sys_identity` gives it, or
 * -ENOENT for a device that is not there. */
static int32_t synth_sys_stat(const struct vpath *vp, struct kstat *kst, int follow)
{
    if (SYSFS_KEYED(vp->sysent) && !sysfs_device_exists(vp->sysent, vp->syskey)) {
        return -ENOENT;
    }
    if (vp->kind == VNODE_SYS_FILE && vp->sysent == SYSFS_PCI_DEVICE &&
        !sysfs_pci_attr_present(vp->syskey, vp->procfile)) {
        return -ENOENT;
    }
    if (vp->kind == VNODE_SYS_FILE && !sysfs_attr_present(vp->sysent, vp->procfile)) {
        return -ENOENT;
    }
    synth_sys_identity(vp, kst, follow);
    return 0;
}

/** Whether `vp` is a node of /sys. */
static int synth_is_sys(const struct vpath *vp)
{
    return vp &&
           (vp->kind == VNODE_SYS_DIR || vp->kind == VNODE_SYS_LINK || vp->kind == VNODE_SYS_FILE);
}

/** The target of the device link `vp`, relative as Linux writes it, into
 * `out` of `cap` bytes; returns its length. */
static int synth_sys_link(const struct vpath *vp, char *out, int cap)
{
    char path[128];
    const char *up = SYSFS_IS_BLOCK(vp->sysent) ? "../.." : "../../..";
    int n = 0, i;

    synth_sys_path(vp->sysent, vp->syskey, path, (int)sizeof(path));
    if (vp->sysent == SYSFS_WLAN_PHY || vp->sysent == SYSFS_WLAN_DRIVER) {
        for (i = 0; path[i] && n < cap; ++i)
            out[n++] = path[i];
        return n;
    }
    for (i = 0; up[i] != '\0' && n < cap; i++) {
        out[n++] = up[i];
    }
    for (i = 4; path[i] != '\0' && n < cap; i++) {
        out[n++] = path[i];
    }
    return n;
}

/** The text of the /sys link `vp` into `out` of `cap` bytes: a registered
 * link's (`sysfs_link_text`), or a device's (`synth_sys_link`); its length,
 * or -ENOENT for a device or a target that is gone. */
static int synth_sys_readlink(const struct vpath *vp, char *out, int cap)
{
    if (vp->sysreg) {
        return sysfs_link_text(vp->sysreg, out, cap);
    }
    if (SYSFS_KEYED(vp->sysent) && !sysfs_device_exists(vp->sysent, vp->syskey)) {
        return -ENOENT;
    }
    return synth_sys_link(vp, out, cap);
}

/**
 * Copy `kst` out to the user's stat64 `st`: with the owner and group the
 * caller's user namespace maps them to (`uid_seen`), which every stat gives
 * the caller, and the times cut to the structure's longs. Returns 0 or
 * -EFAULT.
 */
static int32_t stat64_put(struct user_stat __user *st, const struct kstat *kst)
{
    struct user_stat u;

    k_memset(&u, 0, sizeof(u));
    u.st_dev = kst->st_dev;
    STAT_SET_INO(&u, kst->st_ino);
    u.st_mode = kst->st_mode;
    u.st_nlink = kst->st_nlink;
    u.st_uid = uid_seen(kst->st_uid);
    u.st_gid = gid_seen(kst->st_gid);
    u.st_rdev = kst->st_rdev;
    u.st_size = kst->st_size;
    u.st_blksize = kst->st_blksize;
    u.st_blocks = kst->st_blocks;
    u.st_atime = (unsigned long)kst->st_atime;
    u.st_atime_nsec = kst->st_atime_nsec;
    u.st_mtime = (unsigned long)kst->st_mtime;
    u.st_mtime_nsec = kst->st_mtime_nsec;
    u.st_ctime = (unsigned long)kst->st_ctime;
    u.st_ctime_nsec = kst->st_ctime_nsec;
    return copy_to_user(st, &u, sizeof(u)) != 0 ? -EFAULT : 0;
}

/**
 * The root directory's own three times, into a kstat.
 *
 * The root is a node like any other, and creating an entry in it stamps its
 * times, so it reports them as any directory does; a machine with no file
 * system mounted has nothing to report but the boot time.
 */
static void stat64_root_times(struct kstat *kst)
{
    if (krffs_fs && krffs_fs->node) {
        stat64_node_times(kst, krffs_fs->node);
    } else {
        stat64_boot_times(kst);
    }
}

/**
 * Fill `kst` for the node `r` holds on a mount that is not the KRFFS root,
 * from the type's getattr; 0, or the getattr's error. The device is the one
 * `procfs_mount_dev` gives the mount's entry in the table, a bind mount
 * taking that of the mount it shows: the device numbers of the node the
 * mount was made from, or a number of the mount's own, so a program telling
 * files apart by (st_dev, st_ino) sees two mounts as two devices. A type
 * that does not count blocks reports the size in 512-byte blocks, rounded
 * up.
 */
static int32_t vfs_fill_stat64(struct kstat *kst, const struct vfs_ref *r)
{
    struct vfs_stat st;
    int rc;
    int i;

    k_memset(&st, 0, sizeof(st));
    rc = vfs_getattr(r->mnt, r->node, &st);
    if (rc != 0) {
        return rc;
    }
    k_memset(kst, 0, sizeof(*kst));
    i = vfs_mount_index(vfs_mount_base(r->mnt));
    if (i >= 0) {
        kst->st_dev = procfs_mount_dev(i, vfs_mount_base(r->mnt));
    }
    kst->st_mode = st.mode;
    kst->st_nlink = st.nlink;
    kst->st_uid = st.uid;
    kst->st_gid = st.gid;
    kst->st_rdev = st.rdev;
    kst->st_size = (long long)st.size;
    kst->st_blksize = 4096;
    kst->st_blocks = (long long)(st.blocks_known ? st.blocks : (st.size + 511) / 512);
    kst->st_ino = st.ino;
    kst->st_atime = st.atime_sec;
    kst->st_atime_nsec = (uint32_t)st.atime_nsec;
    kst->st_mtime = st.mtime_sec;
    kst->st_mtime_nsec = (uint32_t)st.mtime_nsec;
    kst->st_ctime = st.ctime_sec;
    kst->st_ctime_nsec = (uint32_t)st.ctime_nsec;
    return 0;
}

/**
 * Fill `kst` from the node `node` of the KRFFS boot root: its type and
 * permission bits, owner, group, size and times, the size in 512-byte
 * blocks, and the inode `krffs_node_ino` gives it. The root node is a
 * directory of mode 0755 whatever its stored mode says: no mkdir made it,
 * and a path can reach it through "..".
 */
static void krffs_fill_stat64(struct kstat *kst, krffs_node_t *node)
{
    k_memset(kst, 0, sizeof(*kst));
    if (node == krffs_fs->node) {
        kst->st_mode = S_IFDIR | 0755;
    } else {
        kst->st_mode = krffs_mode_type(node->mode) | (node->mode & 07777);
    }
    kst->st_nlink = 1; /* KRFFS has no hard links */
    kst->st_uid = node->uid;
    kst->st_gid = node->gid;
    kst->st_size = (long long)node->data_size;
    kst->st_blksize = 4096;
    kst->st_blocks = (node->data_size + 511) / 512;
    kst->st_ino = krffs_node_ino(node);
    stat64_node_times(kst, node);
}

/* The stat of a descriptor, for `synth_fd_stat`; defined with sys_fstat64. */
static int32_t fstat_getattr(fd_entry_t *e, struct kstat *kst);

/**
 * What a followed /proc/PID/fd/N link reports: the stat of descriptor `fd`
 * of process `pid`; 0 on success, -ENOENT when it is absent or closing, or
 * the error of the mounted type's getattr. The entry is copied under the
 * table's lock with what a descriptor in flight holds (`fd_peek_hold`),
 * kept for the length of the stat and given back after it, as Linux's
 * proc_fd_getattr holds the file (fs/proc/fd.c:182 in 6.12): the process
 * may close the descriptor meanwhile, a file's getattr may sleep, and the
 * last close of a pipe, or of another kind `file_ops_unlocked` admits,
 * frees what it names without the big lock. A close
 * that left the copy the last holder is then ended here (`fd_peek_drop`).
 */
static int32_t synth_fd_stat(int pid, int fd, struct kstat *kst)
{
    process_t *p = get_process((uint32_t)pid);
    fd_entry_t e;
    int32_t rc;

    if (!p || !p->fds || fd_peek_hold(p->fds, fd, &e) != 0) {
        return -ENOENT;
    }
    rc = fstat_getattr(&e, kst);
    fd_peek_drop(&e);
    return rc;
}

int32_t fd_stat_ids(int pid, int fd, uint32_t *mnt_id, uint64_t *ino)
{
    struct kstat kst;
    int32_t rc = synth_fd_stat(pid, fd, &kst);

    if (rc == 0) {
        *mnt_id = kst.st_mnt_id;
        *ino = kst.st_ino;
    }
    return rc;
}

/* The inode of /dev/pts, apart from devfs's numbers. */
#define PTS_INO_DIR 3u
/* /dev/log's inode, below the slaves' and /dev's directories' (synthfs.c). */
#define DEV_LOG_INO 4u

/* Slave N's inode is PTS_INO_BASE + N at every stat, so that the
 * comparison of (st_dev, st_ino) ttyname makes matches. */
#define PTS_INO_BASE 0x1000u

/** Fill `kst` for an entry root made under /dev, `id` (devtree.h): its
 * own type, mode, owner, size and times; or -ENOENT once it is removed. */
static int32_t synth_stat_made(int id, struct kstat *kst)
{
    struct devtree_attr a;

    if (devtree_attr(id, &a) != 0) {
        return -ENOENT;
    }
    kst->st_mode = a.mode;
    kst->st_ino = a.ino;
    kst->st_nlink = a.nlink;
    kst->st_uid = a.uid;
    kst->st_gid = a.gid;
    kst->st_size = (int64_t)a.size;
    kst->st_atime = a.atime;
    kst->st_atime_nsec = (uint32_t)a.atime_ns;
    kst->st_mtime = a.mtime;
    kst->st_mtime_nsec = (uint32_t)a.mtime_ns;
    kst->st_ctime = a.ctime;
    kst->st_ctime_nsec = (uint32_t)a.ctime_ns;
    return 0;
}

/** Fill `kst` for a directory of /dev that `vp` names, a row of its table
 * or an entry root made: a directory of /dev's mount with the row's inode,
 * or -ENOENT while the row is absent (`devfs_subdir_present`); a made
 * entry as `synth_stat_made` gives it. */
static int32_t synth_stat_dev_dir(const struct vpath *vp, struct kstat *kst)
{
    kst->st_dev = DEVFS_FSID;
    if (vp->kind == VNODE_DEV_MADE) {
        return synth_stat_made(vp->syskey, kst);
    }
    if (!devfs_subdir_present(vp->devdir)) {
        return -ENOENT;
    }
    kst->st_mode = S_IFDIR | 0755;
    kst->st_ino = synth_dev_dir_at(vp->devdir)->ino;
    return 0;
}

/** Give `kst` the owner of process `pid`'s node under /proc, for the mode
 * already in `st_mode`, as Linux's task_dump_owner does: the effective
 * IDs for a 0555 directory, or for any node of a dumpable process, and
 * root otherwise (`procfs_pid_owner`). BusyBox's ps and top read a
 * process's user from its directory's owner. */
static void synth_pid_owner(int pid, struct kstat *kst)
{
    uint32_t uid, gid;

    if (procfs_pid_owner(pid, kst->st_mode, &uid, &gid) == 0) {
        kst->st_uid = uid;
        kst->st_gid = gid;
    }
}

/** Return the type and permissions of a process directory under /proc.
 * Linux gives fd mode 0500, ns 0511 and the public directories 0555;
 * procfs_pid_owner uses these bits to choose the owner. */
static uint32_t synth_pid_dir_mode(const struct vpath *vp)
{
    if (vp->kind == VNODE_PROC_FD_DIR) {
        return S_IFDIR | 0500;
    }
    if (vp->kind == VNODE_PROC_NS_DIR) {
        return S_IFDIR | 0511;
    }
    return S_IFDIR | 0555;
}

/**
 * Whether /proc/`rpid`/fd/`fd`, or with `info` its fdinfo file, is there for
 * the caller, judged in the order of Linux's lookup: first its directory's
 * permission, the search of /proc/PID/fd by its mode and owner, or for
 * fdinfo Linux's ptrace check (`proc_fd_dir_access`), -EACCES; then -ENOENT
 * when the descriptor is not open (the check readlink makes, so stat and
 * readlink agree); then for a link followed, `follow`, the ptrace check that
 * Linux's `proc_pid_get_link` takes (`procfs_may_read`), -EACCES. An lstat
 * of a link takes no check past the lookup, as Linux's link inode has no
 * permission or getattr of its own. Returns 0 when it is there.
 */
static int32_t synth_fd_visible(int rpid, int fd, int info, int follow)
{
    char scratch[16];
    int32_t rc;

    if (rpid < 0) {
        return -ENOENT;
    }
    rc = proc_fd_dir_access(info ? VNODE_PROC_FDINFO_DIR : VNODE_PROC_FD_DIR, rpid, CRED_X, NULL);
    if (rc != 0) {
        return rc == -ESRCH ? -ENOENT : rc;
    }
    if (procfs_readlink_fd(rpid, fd, scratch, (int)sizeof(scratch)) < 0) {
        return -ENOENT;
    }
    return !follow || procfs_may_read(rpid) ? 0 : -EACCES;
}

/** Whether the file `procfile` of a process's directory takes a write,
 * mode 0644 as Linux gives it: oom_score_adj, the ID maps, setgroups and
 * comm. */
static int synth_proc_writable(int procfile)
{
    return procfile == SYNTH_PROC_OOM_ADJ || procfile == SYNTH_PROC_UID_MAP ||
           procfile == SYNTH_PROC_GID_MAP || procfile == SYNTH_PROC_SETGROUPS ||
           procfile == SYNTH_PROC_COMM;
}

/**
 * Fill `kst`, as `synth_stat_node` has begun it, for the directory of a
 * process or thread that `vp` names, or for /proc/self unfollowed, a link.
 * A task directory has the links `procfs_task_links` counts, and an fd
 * directory the number of descriptors open as its size (`procfs_fd_count`),
 * as Linux's `proc_fd_getattr` gives it (fs/proc/fd.c:350-366). Returns 0,
 * or -ENOENT once the process or thread is gone.
 */
static int32_t synth_stat_pid_dir(const struct vpath *vp, struct kstat *kst, int follow)
{
    int id = procfs_resolve_id(vp);

    if (id < 0) {
        return -ENOENT;
    }
    kst->st_dev = PROCFS_FSID;
    if (vp->kind == VNODE_PROC_PID && vp->pid == SYNTH_PID_SELF && vp->tid == 0 && !follow) {
        kst->st_mode = 0120777; /* S_IFLNK | 0777 */
        kst->st_ino = procfs_ino_dir();
        return 0;
    }
    kst->st_mode = synth_pid_dir_mode(vp);
    kst->st_ino = procfs_ino_pid(id);
    synth_pid_owner(id, kst);
    if (vp->kind == VNODE_PROC_TASK_DIR) {
        kst->st_nlink = procfs_task_links(id);
    }
    if (vp->kind == VNODE_PROC_FD_DIR) {
        int n = procfs_fd_count(id);

        if (n < 0) {
            return n;
        }
        kst->st_size = n;
    }
    return 0;
}

/** Stat one fixed directory of procfs. */
static int synth_proc_dir_stat(struct kstat *kst, uint32_t ino)
{
    kst->st_dev = PROCFS_FSID;
    kst->st_mode = S_IFDIR | 0555;
    kst->st_ino = ino;
    return 0;
}

/** Stat an enabled message's directory or affinity file. */
static int synth_irq_stat(const struct vpath *vp, struct kstat *kst)
{
    if (vp->pid >= 0 && !procfs_irq_exists(vp->pid)) {
        return -ENOENT;
    }
    kst->st_dev = PROCFS_FSID;
    kst->st_mode =
        vp->kind == VNODE_PROC_IRQ_DIR ? S_IFDIR | 0555 : S_IFREG | (vp->procfile ? 0444 : 0644);
    kst->st_ino = procfs_ino_irq(vp->pid, vp->kind == VNODE_PROC_IRQ_DIR ? 0 : vp->procfile + 1);
    return 0;
}

/**
 * Fill `kst` with the attributes stat gives the /dev, /proc or /sys node
 * `vp`, a /proc link followed to what it names when `follow`. Returns 0, or
 * -ENOENT for a node that is not there, a process that has exited, a device
 * that is gone and a pty that is not allocated among them; -ENOTDIR for a
 * name under a node that is not a directory; or -EACCES for a descriptor's
 * entry under /proc/PID/fd the caller may not search, a link of it followed
 * or an entry under /proc/PID/fdinfo of a process the caller may not read
 * (`synth_fd_visible`), or for a followed namespace link of one.
 *
 * Each node has one link, 4096-byte blocks and the boot time for its times;
 * a task directory has the links `procfs_task_links` counts, and a pty
 * slave the access and modification times of its pty. A /proc file is a
 * regular file of mode 0444, root's unless it is a process's, whose files
 * and directories are owned as `synth_pid_owner` has it; oom_score_adj,
 * uid_map, gid_map and setgroups are their owner's to write, mode 0644, as
 * on Linux, and so is a /proc/sys file that can be written. A
 * descriptor's fdinfo file is 0444 (fs/proc/fd.c:383); /proc/disk is 0400,
 * as are a process's syscall, environ and auxv files; its mem file is 0600
 * and its fd directory 0500, whose size is the number of descriptors the
 * process has open (`procfs_fd_count`), as Linux's `proc_fd_getattr` gives
 * it (fs/proc/fd.c:350-366).
 *
 * The /proc links, /proc/self, a process's cwd, root and exe, its namespace
 * links and its descriptors' links, are links of mode 0777 when not
 * followed. A cwd, root or exe link comes here only unfollowed, since the
 * walk follows it; one the caller may not read is there, as on Linux, and
 * one of no process, or the exe of a process with no program, is not.
 * Followed, /proc/self is the process's directory, a namespace link is
 * nsfs's file of the namespace, and a descriptor's link is the stat of the
 * file the descriptor is open on (`synth_fd_stat`).
 *
 * A pty slave, /dev/pts/N, is a character device whose mode, owner and
 * group are kept on the pty, where the login server's chown and chmod put
 * them.
 */
static int32_t synth_stat_node(const struct vpath *vp, struct kstat *kst, int follow)
{
    uint32_t mode = 0, rdev = 0, ino = 0;

    k_memset(kst, 0, sizeof(*kst));
    kst->st_nlink = 1;
    kst->st_blksize = 4096;
    stat64_boot_times(kst);

    switch (vp->kind) {
    case VNODE_DEV_DIR:
        (void)devfs_stat("", &mode, &rdev, &ino);
        kst->st_dev = DEVFS_FSID;
        kst->st_mode = mode;
        kst->st_ino = ino;
        return 0;
    case VNODE_DEV:
        if (devfs_stat(vp->name, &mode, &rdev, &ino) != 0 ||
            devfs_owner(vp->name, &kst->st_uid, &kst->st_gid) != 0) {
            return -ENOENT;
        }
        kst->st_dev = DEVFS_FSID;
        kst->st_mode = mode;
        kst->st_rdev = rdev;
        kst->st_ino = ino;
        return 0;
    case VNODE_PROC_DIR:
        kst->st_dev = PROCFS_FSID;
        kst->st_mode = S_IFDIR | 0555;
        kst->st_ino = procfs_ino_dir();
        return 0;
    case VNODE_PROC_CMDLINE:
        return synth_proc_stat(kst, procfs_ino_cmdline());
    case VNODE_PROC_NET_DIR:
    case VNODE_PROC_NET_STAT_DIR:
    case VNODE_PROC_SYSVIPC_DIR:
        kst->st_dev = PROCFS_FSID;
        kst->st_mode = S_IFDIR | 0555;
        kst->st_ino = vp->kind == VNODE_PROC_NET_DIR        ? procfs_ino_netdir()
                      : vp->kind == VNODE_PROC_NET_STAT_DIR ? procfs_ino_netstatdir()
                                                            : procfs_ino_sysvipcdir();
        return 0;
    case VNODE_PROC_NET_DEV:
        return synth_proc_stat(kst, procfs_ino_netdev());
    case VNODE_PROC_UPTIME:
        return synth_proc_stat(kst, procfs_ino_uptime());
    case VNODE_PROC_STAT_SYS:
        return synth_proc_stat(kst, procfs_ino_statsys());
    case VNODE_PROC_MEMINFO:
        return synth_proc_stat(kst, procfs_ino_meminfo());
    case VNODE_PROC_CPUINFO:
        return synth_proc_stat(kst, procfs_ino_cpuinfo());
    case VNODE_PROC_LOADAVG:
        return synth_proc_stat(kst, procfs_ino_loadavg());
    case VNODE_PROC_DISKSTATS:
        return synth_proc_stat(kst, procfs_ino_diskstats());
    case VNODE_PROC_VERSION:
        return synth_proc_stat(kst, procfs_ino_version());
    case VNODE_PROC_PARTITIONS:
        return synth_proc_stat(kst, procfs_ino_partitions());
    case VNODE_PROC_VMSTAT:
        return synth_proc_stat(kst, procfs_ino_vmstat());
    case VNODE_PROC_SLABINFO:
        return synth_proc_stat(kst, procfs_ino_slabinfo());
    case VNODE_PROC_SWAPS:
        return synth_proc_stat(kst, procfs_ino_swaps());
    case VNODE_PROC_LOCKS:
        return synth_proc_stat(kst, procfs_ino_locks());
    case VNODE_PROC_SYS_FILE: {
        int32_t rc = synth_proc_stat(kst, procfs_ino_sys(vp->sysent));

        if (synth_sys_writable(vp->sysent)) {
            kst->st_mode = S_IFREG | 0644;
        }
        return rc;
    }
    case VNODE_PROC_SYS_DIR:
        kst->st_dev = PROCFS_FSID;
        kst->st_mode = S_IFDIR | 0555;
        kst->st_ino = procfs_ino_sys(vp->sysent);
        return 0;
    case VNODE_PROC_MOUNTS:
        return synth_proc_stat(kst, procfs_ino_mounts());
    case VNODE_PROC_AUDIO:
        return synth_proc_stat(kst, procfs_ino_audio());
    case VNODE_PROC_USB:
        return synth_proc_stat(kst, procfs_ino_usb());
    case VNODE_PROC_LOOP:
        return synth_proc_stat(kst, procfs_ino_loop());
    case VNODE_PROC_FILESYSTEMS:
        return synth_proc_stat(kst, procfs_ino_filesystems());
    case VNODE_PROC_INTERRUPTS:
        return synth_proc_stat(kst, procfs_ino_interrupts());
    case VNODE_PROC_CONSOLE:
        return synth_proc_stat(kst, procfs_ino_console());
    case VNODE_PROC_NET_ROUTE:
        return synth_proc_stat(kst, procfs_ino_netroute());
    case VNODE_PROC_NET_ARP:
        return synth_proc_stat(kst, procfs_ino_netarp());
    case VNODE_PROC_NET_TCP:
        return synth_proc_stat(kst, procfs_ino_nettcp());
    case VNODE_PROC_NET_FILE:
        return synth_proc_stat(kst, procfs_ino_net(vp->procfile));
    case VNODE_PROC_SYSVIPC:
        return synth_proc_stat(kst, procfs_ino_sysvipc(vp->procfile));
    case VNODE_PROC_IRQ_DIR:
    case VNODE_PROC_IRQ_FILE:
        return synth_irq_stat(vp, kst);
    case VNODE_PROC_ACPI_DIR:
        return synth_proc_dir_stat(kst, procfs_ino_acpidir());
    case VNODE_PROC_ACPI_CALL:
        kst->st_dev = PROCFS_FSID;
        kst->st_mode = S_IFREG | 0600;
        kst->st_ino = procfs_ino_acpicall();
        return 0;
    case VNODE_PROC_FS_DIR:
        return synth_proc_dir_stat(kst, procfs_ino_fsdir());
    case VNODE_SYS_DIR:
    case VNODE_SYS_LINK:
    case VNODE_SYS_FILE:
        return synth_sys_stat(vp, kst, follow);
    case VNODE_PROC_DISK:
        kst->st_dev = PROCFS_FSID;
        kst->st_mode = S_IFREG | 0400;
        kst->st_ino = procfs_ino_disk();
        return 0;
    case VNODE_PROC_CWD:
    case VNODE_PROC_ROOT:
    case VNODE_PROC_EXE: {
        char scratch[1];
        int rpid = procfs_resolve_id(vp);

        if (rpid < 0 ||
            (vp->kind == VNODE_PROC_EXE
                 ? procfs_readlink_exe(rpid, scratch, 0)
                 : procfs_readlink_dir(rpid, vp->kind == VNODE_PROC_ROOT, scratch, 0)) == -ENOENT) {
            return -ENOENT;
        }
        kst->st_dev = PROCFS_FSID;
        kst->st_mode = 0120777; /* S_IFLNK | 0777 */
        kst->st_ino = procfs_ino_pid(rpid);
        synth_pid_owner(rpid, kst);
        return 0;
    }
    case VNODE_PROC_NS_LINK: {
        char scratch[1];
        int rpid = procfs_resolve_id(vp);
        int rc = rpid < 0 ? -ENOENT : procfs_readlink_ns(rpid, vp->procfd, scratch, 0);

        if (rc == -ENOENT || (follow && rc < 0)) {
            return rc < 0 ? rc : -ENOENT;
        }
        if (follow) {
            kst->st_dev = NSFS_DEV;
            kst->st_mode = S_IFREG | 0444;
            kst->st_ino = procfs_ns_ino(rpid, vp->procfd);
            kst->st_nlink = 1;
            return 0;
        }
        kst->st_dev = PROCFS_FSID;
        kst->st_mode = 0120777; /* S_IFLNK | 0777 */
        kst->st_ino = procfs_ino_pid(rpid);
        synth_pid_owner(rpid, kst);
        return 0;
    }
    case VNODE_PROC_PID:
    case VNODE_PROC_FD_DIR:
    case VNODE_PROC_FDINFO_DIR:
    case VNODE_PROC_TASK_DIR:
    case VNODE_PROC_NS_DIR:
        return synth_stat_pid_dir(vp, kst, follow);
    case VNODE_PROC_FILE:
        if (procfs_resolve_id(vp) < 0) {
            return -ENOENT;
        }
        kst->st_dev = PROCFS_FSID;
        kst->st_mode = S_IFREG | 0444;
        kst->st_ino = procfs_ino_pid(procfs_resolve_id(vp));
        synth_pid_owner(procfs_resolve_id(vp), kst);
        if (synth_proc_writable(vp->procfile)) {
            kst->st_mode = S_IFREG | 0644;
        } else if (procfs_owner_mode(vp->procfile) != 0) {
            kst->st_mode = S_IFREG | procfs_owner_mode(vp->procfile);
        }
        return 0;
    case VNODE_PROC_FDINFO: {
        int rpid = procfs_resolve_id(vp);
        int32_t rc = synth_fd_visible(rpid, vp->procfd, 1, 0);

        if (rc != 0) {
            return rc;
        }
        kst->st_dev = PROCFS_FSID;
        kst->st_mode = S_IFREG | 0444;
        kst->st_ino = procfs_ino_pid(rpid);
        synth_pid_owner(rpid, kst);
        return 0;
    }
    case VNODE_PROC_FD_LINK: {
        int rpid = procfs_resolve_id(vp);
        int32_t rc = synth_fd_visible(rpid, vp->procfd, 0, follow);

        if (rc != 0) {
            return rc;
        }
        if (follow) {
            return synth_fd_stat(rpid, vp->procfd, kst);
        }
        kst->st_dev = PROCFS_FSID;
        kst->st_mode = 0120777; /* S_IFLNK | 0777 */
        kst->st_ino = procfs_ino_pid(rpid);
        synth_pid_owner(rpid, kst);
        return 0;
    }
    case VNODE_DEV_LINK:
        kst->st_dev = DEVFS_FSID;
        kst->st_mode = 0120777; /* S_IFLNK | 0777 */
        kst->st_ino = DEV_LOG_INO;
        kst->st_size = (int64_t)k_strlen(SYNTH_DEV_LOG_TARGET);
        return 0;
    case VNODE_PTS_DIR:
        kst->st_dev = DEVFS_FSID;
        kst->st_mode = S_IFDIR | 0755;
        kst->st_ino = PTS_INO_DIR;
        return 0;
    case VNODE_DEV_SUBDIR:
    case VNODE_DEV_MADE:
        return synth_stat_dev_dir(vp, kst);
    case VNODE_PTS: {
        /* Linux's major for a Unix98 pty slave is 136. */
        struct pty *pt = pty_get(vp->pts_index);
        if (!pt) {
            return -ENOENT;
        }
        kst->st_dev = DEVFS_FSID;
        kst->st_mode = 0020000u | (uint32_t)(pt->mode & 0777u);
        kst->st_uid = pt->uid;
        kst->st_gid = pt->gid;
        kst->st_rdev = (136u << 8) | (uint32_t)vp->pts_index;
        kst->st_ino = PTS_INO_BASE + (uint32_t)vp->pts_index;
        kst->st_atime = pt->atime;
        kst->st_mtime = pt->mtime;
        return 0;
    }
    case VNODE_ENOTDIR:
        return -ENOTDIR;
    default:
        return procfs_ino_global(vp->kind) ? synth_proc_stat(kst, procfs_ino_global(vp->kind))
                                           : -ENOENT;
    }
}

/**
 * Fill `kst` for the node `vp` as a lookup of its path finds it
 * (`synth_stat_node`). A lookup that finds a file of /proc/<pid>, an fdinfo
 * file, a process's fd, fdinfo or task directory, or one of its links gives
 * the descriptors open on it the owner it reports, as Linux's walk sets
 * the owner of the inode they share (`procfs_lookup_owner`); the fstat of a
 * descriptor takes `synth_stat_node`, which sets nothing.
 */
static int32_t synth_stat(const struct vpath *vp, struct kstat *kst, int follow)
{
    int32_t rc = synth_stat_node(vp, kst, follow);

    if (rc == 0) {
        procfs_lookup_owner(vp);
    }
    return rc;
}

/**
 * Whether the node `r` a walk reached is a directory: 0, -ENOTDIR for
 * anything else, or the error reading the node gave. A /dev or /proc node is
 * judged by the stat `synth_stat` gives it, a /proc link followed when
 * `follow`, so /proc/self and a descriptor's link to a directory are
 * directories only when followed; a node of the KRFFS root by its mode and
 * the root itself by its identity; and a node of a mounted type by its
 * attributes.
 */
static int32_t ref_dir_check(const struct vfs_ref *r, int follow)
{
    const struct vpath *vp = vfs_synth_vpath(r->mnt, r->node);
    struct kstat kst;
    struct vfs_stat vst;
    int32_t rc;

    if (vp) {
        rc = synth_stat(vp, &kst, follow);
        return rc != 0 ? rc : ((kst.st_mode & S_IFMT) == S_IFDIR ? 0 : -ENOTDIR);
    }
    if (mnt_is_root(r->mnt)) {
        const krffs_node_t *node = r->node;

        return (node == krffs_fs->node || KRFFS_IS_DIR(node->mode)) ? 0 : -ENOTDIR;
    }
    rc = vfs_getattr(r->mnt, r->node, &vst);
    return rc != 0 ? rc : ((vst.mode & S_IFMT) == S_IFDIR ? 0 : -ENOTDIR);
}

/**
 * Whether the name `leaf` in the held directory `parent`, which is not in
 * /dev or /proc, is a directory: 0, -ENOTDIR for anything else, a symbolic
 * link to a directory among them, since the link is not followed, or the
 * lookup's error, -ENOENT for a name that is not there. It is what a slash
 * after the last name asks of an unlink or a rename, which Linux judges by
 * the name itself.
 */
static int32_t leaf_dir_check(const struct vfs_ref *parent, const char *leaf)
{
    struct vfs_ref at;
    int32_t rc = parent->mnt->type->ops->lookup(parent->mnt, parent->node, leaf, &at.node);

    if (rc != 0) {
        return rc;
    }
    at.mnt = parent->mnt;
    rc = ref_dir_check(&at, 0);
    if (parent->mnt->type->ops->put) {
        parent->mnt->type->ops->put(parent->mnt, at.node);
    }
    return rc;
}

/**
 * The node checked is the node returned, so a call that goes on with `ref` acts
 * on what was checked; execve opens its program and interpreters through it
 * (`exec_open` in `syscall_proc.c`), since a second walk could meet a symbolic
 * link replaced in between.
 */
int32_t resolve_named(const char *name, int walk, int dir, struct vfs_ref *ref)
{
    return resolve_named_real(name, walk, dir, ref, NULL);
}

int32_t resolve_named_real(const char *name, int walk, int dir, struct vfs_ref *ref, char *real)
{
    return resolve_named_entry(name, walk, dir, ref, real, NULL);
}

int32_t resolve_named_entry(const char *name, int walk, int dir, struct vfs_ref *ref, char *real,
                            struct vfs_name **entry)
{
    int32_t rc =
        vfs_resolve_entry(name, dir ? walk & ~VFS_WALK_NOFOLLOW : walk, ref, real, entry, NULL);

    if (rc == 0 && dir) {
        rc = ref_dir_check(ref, 1);
        if (rc != 0) {
            if (entry) {
                vfs_name_put(*entry);
                *entry = NULL;
            }
            if (walk & VFS_WALK_LOCK) {
                vfs_walk_unlock(ref->mnt);
            }
            vfs_ref_release(ref);
        }
    }
    return rc;
}

/**
 * The open with O_CREAT, which asks for a file, of a path that named a
 * directory by its end (`end`), which no file can be: -EISDIR once the
 * directories before its last name are walked, as Linux answers, and for a
 * last "." or "..", which names a directory that exists, -EEXIST under
 * O_EXCL; the walk's error, -ENOTDIR for a "." after a name that is not a
 * directory among them, comes first.
 */
static int32_t open_create_dir_end(const char *name, int flags, int end)
{
    char leaf[VFS_NAME_MAX + 1];
    struct vfs_ref ref;
    int32_t rc;

    if (end == PATH_END_SLASH && name[0] != '\0') {
        rc = vfs_resolve(0, name, VFS_WALK_PARENT, &ref, leaf, sizeof(leaf));
        if (rc == 0) {
            vfs_ref_release(&ref);
        }
        return rc != 0 ? rc : -EISDIR;
    }
    rc = resolve_named(name, 0, end, &ref);
    if (rc != 0) {
        return rc;
    }
    vfs_ref_release(&ref);
    return (flags & O_EXCL) ? -EEXIST : -EISDIR;
}

/**
 * open(2), openat(2) and creat(2) of the path `name`, leading '/' stripped,
 * which named a directory by its end when `end` is not 0
 * (`path_names_dir`): such a path must reach a directory, so the open acts
 * as with O_DIRECTORY and follows a final symbolic link whatever O_NOFOLLOW
 * says, as Linux's does, and with O_CREAT, which asks for a file, it is
 * `open_create_dir_end`'s. Any other path is `do_open`'s.
 */
static int32_t open_named(const char *name, int flags, uint32_t mode, int end)
{
    if (end && (flags & (O_CREAT | O_PATH)) == O_CREAT) {
        return open_create_dir_end(name, flags, end);
    }
    if (end) {
        flags = (flags | O_DIRECTORY) & ~O_NOFOLLOW;
    }
    return do_open(name, flags, mode);
}

/**
 * An O_PATH descriptor on the synthetic node `vp`, whose canonical path is
 * `name`, leading '/' stripped. A directory is FD_TYPE_SYNTH_DIR, as an open
 * of it makes, and anything else FD_TYPE_SYNTH_PATH, which keeps a copy of
 * `vp` for fstat and fstatfs, the owner a lookup last gave a /proc node
 * whose inode keeps it (`procfs_owner_keep`), and `name` for /proc/PID/fd.
 * Nothing is opened: a device does not learn of it and a /proc file takes
 * no snapshot.
 *
 * The /proc links are classified, not resolved, so the O_NOFOLLOW choice is
 * made here: /proc/self is the process directory unless O_NOFOLLOW names the
 * link, and a /proc/PID/fd link follows its descriptor to the mounted or
 * synthetic object unless O_NOFOLLOW names the link itself. O_DIRECTORY on
 * anything but a directory is -ENOTDIR, and a node that is gone, a process
 * that exited among them, is -ENOENT.
 */
static int32_t synth_open_path(const struct vpath *vp, const char *name, int flags)
{
    int follow = !(flags & O_NOFOLLOW);
    struct kstat kst;
    struct synth_path *copy;
    fd_entry_t *e;
    int fd;

    if (follow && vp->kind == VNODE_PROC_FD_LINK) {
        return proc_fd_open_path(vp, flags);
    }
    if (synth_stat(vp, &kst, follow) != 0) {
        return -ENOENT;
    }
    if ((kst.st_mode & S_IFMT) == S_IFDIR) {
        return synth_open_dir(vp, (flags & O_CLOEXEC) | O_PATH);
    }
    if (flags & O_DIRECTORY) {
        return -ENOTDIR;
    }
    copy = kzalloc(sizeof(*copy));
    if (!copy) {
        return -ENOMEM;
    }
    fd = fd_alloc(current_proc->fds);
    if (fd < 0) {
        kfree(copy);
        return fd;
    }
    copy->vp = *vp;
    procfs_owner_keep(&copy->own, vp);
    e = fd_slot(current_proc->fds, fd);
    e->type = FD_TYPE_SYNTH_PATH;
    e->node = NULL;
    e->sock = NULL;
    e->ops = NULL;
    e->priv = copy;
    e->dev = NULL;
    ofd_record_path(e->ofd, name);
    e->flags = FD_FLAG_PATH | ((flags & O_CLOEXEC) ? FD_FLAG_CLOEXEC : 0);
    fd_install(current_proc->fds, fd);
    return fd;
}

/**
 * An O_PATH descriptor on the node `ref` resolved to, on the KRFFS boot root
 * or a mounted type: a directory as FD_TYPE_DIR and anything else as
 * FD_TYPE_FILE, with FD_FLAG_PATH and neither access flag. The description
 * takes the resolution's hold and a reference on the mount, as an open's
 * does, and no handle, since the type's `open` does not run: a device node
 * of ext2 is not refused, and its last close runs no `release`. On a type
 * that keeps a node only while it is open, mounted KRFFS, the description
 * also takes a pin through the type's `pin`, which its last close drops, so
 * an unlink leaves the node to the descriptor as it would to an open one.
 * O_DIRECTORY on anything but a directory is -ENOTDIR. The reference is
 * released on every error, a failed pin's among them.
 */
static int32_t path_install(struct vfs_ref *ref, const char *name, int flags)
{
    krffs_node_t *node = ref->node;
    fd_entry_t *entry;
    int is_dir;
    int fd;

    if (mnt_is_root(ref->mnt)) {
        is_dir = node == krffs_fs->node || KRFFS_IS_DIR(node->mode);
    } else {
        struct vfs_stat vst;
        int rc = vfs_getattr(ref->mnt, ref->node, &vst);

        if (rc != 0) {
            vfs_ref_release(ref);
            return rc;
        }
        is_dir = (vst.mode & S_IFMT) == S_IFDIR;
    }
    if (!is_dir && (flags & O_DIRECTORY)) {
        vfs_ref_release(ref);
        return -ENOTDIR;
    }
    fd = fd_alloc(current_proc->fds);
    if (fd < 0) {
        vfs_ref_release(ref);
        return fd;
    }
    entry = fd_slot(current_proc->fds, fd);
    if (!mnt_is_root(ref->mnt) && ref->mnt->type->ops->pin) {
        int rc = ref->mnt->type->ops->pin(ref->mnt, ref->node, &entry->ofd->pin);

        if (rc != 0) {
            fd_free(current_proc->fds, fd);
            vfs_ref_release(ref);
            return rc;
        }
    }
    entry->type = is_dir ? FD_TYPE_DIR : FD_TYPE_FILE;
    entry->node = (mnt_is_root(ref->mnt) && node != krffs_fs->node) ? node : NULL;
    if (!mnt_is_root(ref->mnt)) {
        ofd_record_path(entry->ofd, name);
    }
    entry->ofd->vmnt = ref->mnt;
    entry->ofd->vnode = ref->node;
    entry->ofd->fhandle = NULL;
    vfs_desc_open(ref->mnt);
    vfs_leave(ref->mnt);
    entry->flags = FD_FLAG_PATH | ((flags & O_CLOEXEC) ? FD_FLAG_CLOEXEC : 0);
    fd_install(current_proc->fds, fd);
    return fd;
}

static int32_t proc_fd_open_path_from(const struct vpath *vp, process_t *p, fd_entry_t *src,
                                      int flags);

/**
 * Open the object of a proc descriptor link with O_PATH and no device open.
 * Mounted files keep their inode through path_install; synthetic objects
 * keep the source description, which O_PATH cannot read or advance.
 * The caller has the kernel lock. Return a descriptor or a negative errno.
 */
static int32_t proc_fd_open_path(const struct vpath *vp, int flags)
{
    process_t *p;
    fd_entry_t src;
    int32_t rc;

    rc = proc_fd_entry(vp, &p, &src);
    if (rc != 0) {
        return rc;
    }
    rc = proc_fd_open_path_from(vp, p, &src, flags);
    fd_peek_drop(&src);
    return rc;
}

/** `proc_fd_open_path` of the copy `src` of descriptor `vp->procfd` of
 * `p`. */
static int32_t proc_fd_open_path_from(const struct vpath *vp, process_t *p, fd_entry_t *src,
                                      int flags)
{
    struct vfs_ref ref;
    struct vfs_mount *mnt;
    struct open_file *held;
    int32_t rc;
    int fd;

    rc = proc_fd_xdev(src, current_proc->walk_replay_mnt);
    if (rc != 0) {
        return rc;
    }
    if (src->ofd && src->ofd->vmnt && (src->type == FD_TYPE_FILE || src->type == FD_TYPE_DIR)) {
        rc = proc_fd_file(vp, &ref, &held);
        if (rc != 0) {
            return rc;
        }
        mnt = ref.mnt;
        fd = path_install(&ref, held->dirpath ? path_name(held->dirpath) : "", flags);
        if (fd >= 0) {
            fd_slot(current_proc->fds, fd)->ofd->name = vfs_name_ref(held->name);
        }
        vfs_walk_unlock(mnt);
        fd_ofd_unref(held);
        return fd;
    }
    if (src->type != FD_TYPE_SYNTH_DIR && src->type != FD_TYPE_SYNTH_PATH) {
        return -ENOENT;
    }
    if ((flags & O_DIRECTORY) && src->type != FD_TYPE_SYNTH_DIR) {
        return -ENOTDIR;
    }
    fd = proc_fd_share(p->fds, vp->procfd, flags | PROC_FD_SHARE_PATH);
    return fd;
}

/**
 * open(2) and openat(2) with O_PATH: a descriptor that names the file at
 * `name`, leading '/' stripped, and opens nothing.
 *
 * Linux keeps O_DIRECTORY, O_NOFOLLOW and O_CLOEXEC beside O_PATH and drops
 * every other flag, so nothing is created or truncated. The directories on
 * the way need search permission and the file itself none. A symbolic link
 * at the end is followed unless O_NOFOLLOW, which names the link. The
 * descriptor serves fstat, fstatfs, fchdir, the *at calls as a directory
 * and with AT_EMPTY_PATH, the two duplications, F_GETFD, F_SETFD, F_GETFL
 * and close; read, write and the other calls on a descriptor are -EBADF.
 */
static int32_t open_path(const char *name, int flags)
{
    struct vfs_ref ref;
    struct vfs_name *entry = NULL;
    struct vfs_mount *locked;
    const struct vpath *vp;
    int walk = VFS_WALK_LOCK;
    int32_t rc;

    flags &= O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC;
    if (flags & O_NOFOLLOW) {
        walk |= VFS_WALK_NOFOLLOW;
    }
    if (!fd_can_alloc(current_proc->fds)) {
        return -EMFILE;
    }
    rc = vfs_resolve_entry(name, walk, &ref, NULL, &entry, NULL);
    if (rc != 0) {
        return rc;
    }
    locked = ref.mnt;
    vp = vfs_synth_vpath(ref.mnt, ref.node);
    if (vp) {
        struct vpath vpc = *vp;
        int permanent = mount_permanent_at(ref.mnt);

        vfs_walk_unlock(locked);
        vfs_name_put(entry);
        rc = synth_open_path(&vpc, name, flags);
        synth_mount_record(rc, &ref, &vpc, flags);
        vfs_ref_release(&ref);
        synth_dir_record(rc, permanent, name);
        return rc;
    }
    rc = path_install(&ref, name, flags);
    if (rc >= 0) {
        fd_slot(current_proc->fds, rc)->ofd->name = entry;
        entry = NULL;
    }
    vfs_name_put(entry);
    vfs_walk_unlock(locked);
    return rc;
}

static int synth_readlink(const struct vpath *vp, char *link);
static int32_t readlink_put(int n, char *link, char __user *ubuf, uint32_t bufsiz);
static int user_path_empty(const char __user *upath);

/**
 * The core of readlink(2) and readlinkat(2): copy the target of the link at
 * `name`, leading '/' stripped, into the user's `ubuf` of `bufsiz` bytes,
 * cut to fit and with no NUL, as the call's contract has it; the bytes
 * copied, or a negative errno. musl's readlink returns the count as it is,
 * and its ttyname_r appends the NUL and takes a full buffer as truncation.
 *
 * The links are those of a mounted type that has them, read through the
 * type, and the synthetic ones: /proc/self, the number of the caller's
 * process as its PID namespace sees it; a process's exe, cwd and root, its
 * namespace links, and its descriptors' links, which ttyname reads and
 * which are -EACCES to a caller that may not read the process's
 * descriptors; a /sys device link, relative as Linux writes it; and
 * /dev/log. A process that has exited, or a device that is gone, is
 * -ENOENT.
 *
 * A NULL buffer or a size of 0 is -EINVAL, and so is a node that is not a
 * link. A path that named a directory by its end, `dir`, follows a final
 * link and must reach a directory (`resolve_named`), which is no link:
 * -EINVAL, as on Linux. The target is made in a path page, up to the
 * resolver's own bound, and no page to be had is -ENOMEM.
 */
static int32_t do_readlink(const char *name, char __user *ubuf, uint32_t bufsiz, int dir)
{
    struct vpath vp;
    struct vfs_ref ref;
    const struct vpath *svp;
    char *link;
    int n, kind;

    if (!ubuf || bufsiz == 0) {
        return -EINVAL;
    }
    kind = (int)resolve_named(name, VFS_WALK_NOFOLLOW, dir, &ref);
    if (kind != 0) {
        return kind;
    }
    if (dir) {
        vfs_ref_release(&ref);
        return -EINVAL;
    }
    link = path_buf_get();
    if (!link) {
        vfs_ref_release(&ref);
        return -ENOMEM;
    }
    svp = vfs_synth_vpath(ref.mnt, ref.node);
    if (!svp || svp->kind == VNODE_DEV_MADE) {
        n = ref.mnt->type->ops->readlink
                ? ref.mnt->type->ops->readlink(ref.mnt, ref.node, link, VFS_PATH_MAX)
                : -EINVAL;
        vfs_ref_release(&ref);
    } else {
        vp = *svp;
        vfs_ref_release(&ref);
        n = synth_readlink(&vp, link);
    }
    return readlink_put(n, link, ubuf, bufsiz);
}

/**
 * The target of the synthetic link `vp` into `link`, a path page, with no
 * NUL: its length, -EINVAL for a node that is no link, or the error
 * `do_readlink` describes.
 */
static int synth_readlink(const struct vpath *vp, char *link)
{
    int n, pid;
    int kind = vp->kind;

    if (kind == VNODE_PROC_PID && vp->pid == SYNTH_PID_SELF && vp->tid == 0) {
        uint32_t uv;
        char t[12];
        int tn = 0;
        pid = procfs_resolve_pid(vp->pid);
        n = 0;
        uv = pid < 0 ? 0u : pid_seen((uint32_t)pid);
        if (pid < 0) {
            n = -ENOENT;
        } else if (uv == 0) {
            link[n++] = '0';
        } else {
            while (uv) {
                t[tn++] = (char)('0' + uv % 10u);
                uv /= 10u;
            }
            while (tn) {
                link[n++] = t[--tn];
            }
        }
    } else if (kind == VNODE_PROC_FD_LINK) {
        pid = procfs_resolve_id(vp);
        n = pid < 0                 ? -ENOENT
            : !procfs_may_read(pid) ? -EACCES
                                    : procfs_readlink_fd(pid, vp->procfd, link, VFS_PATH_MAX);
    } else if (kind == VNODE_PROC_NS_LINK) {
        pid = procfs_resolve_id(vp);
        n = pid < 0 ? -ENOENT : procfs_readlink_ns(pid, vp->procfd, link, VFS_PATH_MAX);
    } else if (kind == VNODE_PROC_EXE) {
        pid = procfs_resolve_id(vp);
        n = pid < 0 ? -ENOENT : procfs_readlink_exe(pid, link, VFS_PATH_MAX);
    } else if (kind == VNODE_PROC_CWD || kind == VNODE_PROC_ROOT) {
        pid = procfs_resolve_id(vp);
        n = pid < 0 ? -ENOENT
                    : procfs_readlink_dir(pid, kind == VNODE_PROC_ROOT, link, VFS_PATH_MAX);
    } else if (kind == VNODE_SYS_LINK) {
        n = synth_sys_readlink(vp, link, VFS_PATH_MAX);
    } else if (kind == VNODE_DEV_LINK) {
        n = (int)k_strlen(SYNTH_DEV_LOG_TARGET);
        k_memcpy(link, SYNTH_DEV_LOG_TARGET, (uint32_t)n);
    } else {
        n = -EINVAL;
    }
    return n;
}

/**
 * Finish a readlink: copy the `n` bytes of `link` that fit the user's
 * `ubuf` of `bufsiz` bytes and give the page back; the count, or `n` itself
 * when it is an errno, or -EFAULT.
 */
static int32_t readlink_put(int n, char *link, char __user *ubuf, uint32_t bufsiz)
{
    if (n >= 0 && (uint32_t)n > bufsiz) {
        n = (int)bufsiz;
    }
    if (n >= 0 && copy_to_user(ubuf, link, (uint32_t)n) != 0) {
        n = -EFAULT;
    }
    path_buf_put(link);
    return n;
}

/**
 * `do_readlink` for a caller without the big lock: a link the name cache
 * finds by a path absolute or relative to the working directory
 * (`vfs_resolve_cached`, `path_from_user_unlocked`), on a type that sets
 * `reads_unlocked`, whose `readlink` reads the link under its own lock, as
 * Linux's readlinkat walks a path under RCU first and reads an ext2 link
 * under no global lock (fs/stat.c:504-538, fs/namei.c:2651-2653). The
 * count copied, a negative errno, or CALL_LOCKED with nothing done, for
 * `do_readlink` under the big lock: a path that names a directory by its
 * end among them.
 */
static int32_t readlink_unlocked(int dirfd, const char __user *upath, char __user *ubuf,
                                 uint32_t bufsiz)
{
    struct vfs_ref ref;
    char *path, *link;
    int dir = 0, n;
    int32_t rc;

    if (!ubuf || bufsiz == 0) {
        return -EINVAL;
    }
    rc = path_from_user_unlocked(dirfd, upath, &path, &dir);
    if (rc != 0) {
        return rc;
    }
    if (dir) {
        path_buf_put(path);
        return CALL_LOCKED;
    }
    rc = vfs_resolve_cached(path, VFS_WALK_NOFOLLOW, &ref, NULL);
    path_buf_put(path);
    if (rc != 0) {
        return rc == VFS_CACHED_MISS ? CALL_LOCKED : rc;
    }
    link = NULL;
    if (vfs_mount_base(ref.mnt)->type->reads_unlocked && ref.mnt->type->ops->readlink) {
        link = path_buf_get();
        if (!link) {
            vfs_ref_release(&ref);
            return -ENOMEM;
        }
    }
    n = link ? ref.mnt->type->ops->readlink(ref.mnt, ref.node, link, VFS_PATH_MAX) : CALL_LOCKED;
    vfs_ref_release(&ref);
    return link ? readlink_put(n, link, ubuf, bufsiz) : CALL_LOCKED;
}

/**
 * The target of the link at the user's path `upath`, relative to `dirfd`,
 * into the user's `ubuf` of `bufsiz` bytes (`do_readlink`): without the big
 * lock where `readlink_unlocked` finishes it, and under it otherwise. The
 * count copied or a negative errno.
 */
static int32_t readlink_path(int dirfd, const char __user *upath, char __user *ubuf,
                             uint32_t bufsiz)
{
    char *path;
    int dir, big;
    int32_t rc;

    big = !bkl_held();
    if (big) {
        rc = readlink_unlocked(dirfd, upath, ubuf, bufsiz);
        if (rc != CALL_LOCKED) {
            return rc;
        }
        bkl_enter();
    }
    rc = path_from_user_dir(dirfd, upath, &path, &dir);
    if (rc == 0) {
        rc = do_readlink(path_name(path), ubuf, bufsiz, dir);
        path_buf_put(path);
    }
    if (big) {
        bkl_exit();
    }
    return rc;
}

/**
 * readlinkat(2) with an empty path: the target of the link the descriptor
 * `dirfd` is open on, an O_PATH descriptor opened with O_NOFOLLOW, into the
 * user's `ubuf` of `bufsiz` bytes, as `do_readlink` copies it.
 * steam-runtime-tools resolves each component of a path this way. A
 * descriptor on anything but a link, AT_FDCWD among them, is -ENOENT, as
 * on Linux; one not open is -EBADF.
 */
static int32_t fd_readlink(int dirfd, char __user *ubuf, uint32_t bufsiz)
{
    fd_entry_t *entry;
    struct vfs_mount *mnt;
    struct vfs_stat vst;
    char *link;
    int n;

    if (!ubuf || bufsiz == 0) {
        return -EINVAL;
    }
    if (dirfd == AT_FDCWD) {
        return -ENOENT;
    }
    entry = fd_get(current_proc->fds, dirfd);
    if (!entry) {
        return -EBADF;
    }
    if (entry->type == FD_TYPE_SYNTH_PATH && entry->priv) {
        link = path_buf_get();
        if (!link) {
            return -ENOMEM;
        }
        n = synth_readlink((const struct vpath *)entry->priv, link);
        return readlink_put(n == -EINVAL ? -ENOENT : n, link, ubuf, bufsiz);
    }
    if (entry->type != FD_TYPE_FILE || !entry->ofd || !entry->ofd->vmnt ||
        mnt_is_root(entry->ofd->vmnt)) {
        return -ENOENT;
    }
    mnt = entry->ofd->vmnt;
    n = vfs_getattr(mnt, entry->ofd->vnode, &vst);
    if (n != 0) {
        return n;
    }
    if ((vst.mode & S_IFMT) != 0120000u || !mnt->type->ops->readlink) { /* S_IFLNK */
        return -ENOENT;
    }
    link = path_buf_get();
    if (!link) {
        return -ENOMEM;
    }
    n = mnt->type->ops->readlink(mnt, entry->ofd->vnode, link, VFS_PATH_MAX);
    return readlink_put(n, link, ubuf, bufsiz);
}

/**
 * readlink(2): copy the target of the link at the user's path in argument 0
 * into the buffer in argument 1, of the size in argument 2 (`readlink_path`);
 * the bytes copied, or a negative errno, -EFAULT for a NULL path. The call
 * runs without the big lock, which `readlink_path` takes for what the name
 * cache leaves.
 */
long sys_readlink(isr_params_t *params)
{
    const char __user *upath = (const char __user *)SC_ARG(params, 0);

    if (!current_proc || !upath) {
        return -EFAULT;
    }
    return readlink_path(AT_FDCWD, upath, (char __user *)SC_ARG(params, 1),
                         USER_LEN32(SC_ARG(params, 2)));
}

/**
 * readlinkat(2): readlink(2) of a path relative to the directory descriptor
 * in argument 0, or to the working directory for AT_FDCWD; the path, the
 * buffer and its size are arguments 1 to 3. An empty path reads the link
 * the descriptor itself is open on (`fd_readlink`), as Linux does without
 * a flag, under the big lock; any other runs without it where the name
 * cache finishes it (`readlink_path`).
 */
long sys_readlinkat(isr_params_t *params)
{
    int dirfd = (int)SC_ARG(params, 0);
    const char __user *upath = (const char __user *)SC_ARG(params, 1);
    char __user *ubuf = (char __user *)SC_ARG(params, 2);
    uint32_t bufsiz = USER_LEN32(SC_ARG(params, 3));
    int32_t rc;
    int big;

    if (!current_proc || !upath) {
        return -EFAULT;
    }
    rc = user_path_empty(upath);
    if (rc == 0) {
        return readlink_path(dirfd, upath, ubuf, bufsiz);
    }
    if (rc < 0) {
        return rc;
    }
    big = !bkl_held();
    if (big) {
        bkl_enter();
    }
    rc = fd_readlink(dirfd, ubuf, bufsiz);
    if (big) {
        bkl_exit();
    }
    return rc;
}

/** Whether the synthetic node `vp` is a link when not followed, by the mode
 * `synth_stat` gives it: a /proc link, a /sys device link, /dev/log or a
 * link made under /dev. */
static int synth_is_link(const struct vpath *vp)
{
    struct kstat lst;

    return synth_stat(vp, &lst, 0) == 0 && (lst.st_mode & 0170000u) == 0120000u;
}

/**
 * The attributes stat64 and lstat64 report for the user's `path`, a final
 * symbolic link followed when `follow`, copied out to `st` with the owner
 * and group the caller's user namespace sees; 0, or a negative errno:
 * -EFAULT for a NULL argument or a bad buffer, the walk's error for a
 * missing name or a directory on the way that is not one or may not be
 * searched, or -ENOTDIR for a path that names a directory by its end, which
 * follows a final link whatever `follow` says, and reaches anything else.
 *
 * The KRFFS root has a fixed identity, inode 1 and mode 0755, and times of
 * its own, which an entry made in it stamps, so that a program watching the
 * directory sees the change.
 *
 * The walk takes the lock of the mount it ends on, where the type asks for
 * one, and keeps it until the attributes are read: a node of a mounted
 * KRFFS volume is its offset, which a growing write that held the lock would
 * move, and a read at the offset the walk found would report whatever the
 * volume put there since.
 */
static int32_t do_stat64(const char __user *path, struct user_stat __user *st, int follow)
{
    struct kstat kst;
    char *pathbuf;
    int dir = 0;

    if (!current_proc || !path || !st) {
        return -EFAULT;
    }

    {
        int rc = path_from_user_dir(AT_FDCWD, path, &pathbuf, &dir);

        if (rc != 0) {
            return rc;
        }
        follow |= dir;
    }

    k_memset(&kst, 0, sizeof(kst));

    {
        struct vfs_ref ref;
        const struct vpath *vp;
        int rc =
            vfs_resolve(0, pathbuf, VFS_WALK_LOCK | (follow ? 0 : VFS_WALK_NOFOLLOW), &ref, 0, 0);

        path_buf_put(pathbuf);
        if (rc != 0) {
            return rc;
        }
        vp = vfs_synth_vpath(ref.mnt, ref.node);
        if (vp) {
            rc = synth_stat(vp, &kst, follow);
        } else if (!mnt_is_root(ref.mnt)) {
            rc = vfs_fill_stat64(&kst, &ref);
        } else if ((krffs_node_t *)ref.node == krffs_fs->node) {
            kst.st_mode = S_IFDIR | 0755;
            kst.st_nlink = 1;
            kst.st_ino = 1;
            kst.st_blksize = 4096;
            stat64_root_times(&kst);
        } else {
            krffs_fill_stat64(&kst, (krffs_node_t *)ref.node);
        }
        vfs_walk_unlock(ref.mnt);
        vfs_ref_release(&ref);
        if (rc != 0) {
            return rc;
        }
    }

    if (dir && (kst.st_mode & S_IFMT) != S_IFDIR) {
        return -ENOTDIR;
    }
    return stat64_put(st, &kst);
}

/**
 * stat64(2), and stat(2) on x86-64: copy out the attributes of the file at
 * the user's path in argument 0, through a final symbolic link, to the
 * buffer in argument 1 (`do_stat64`); 0 or a negative errno. musl's stat
 * comes here on x86-64, and on i386 when statx is missing.
 */
long sys_stat64(isr_params_t *params)
{
    return do_stat64((const char __user *)SC_ARG(params, 0),
                     (struct user_stat __user *)SC_ARG(params, 1), 1);
}

/**
 * lstat64(2), and lstat(2) on x86-64: as stat64(2), but a final symbolic
 * link, on /proc or on a mounted type, is reported itself. A link the path
 * names as a directory by its end is followed either way
 * (`path_names_dir`).
 */
long sys_lstat64(isr_params_t *params)
{
    return do_stat64((const char __user *)SC_ARG(params, 0),
                     (struct user_stat __user *)SC_ARG(params, 1), 0);
}

/*
 * The struct statfs a statfs fills, as Linux writes it and musl reads it
 * (`struct user_statfs`, in the architecture's uabi.h). `f_bsize` is the
 * unit `f_blocks` and `f_bfree` count in. KRFFS has no blocks, so the unit
 * is chosen: 1024, the unit `df` prints in by default, so that `df` prints
 * the kernel's counts as they are.
 */

#define STATFS_BSIZE 1024u

/* The magic numbers statfs reports for the KRFFS boot root, for /proc,
 * /dev and /sys, and for the file systems Linux keeps the objects of pipes,
 * sockets, namespace files and other anonymous descriptors on; a mounted
 * volume, the tmpfs at /dev/shm among them, answers with its own statfs.
 * The synthetic ones have Linux's numbers, so a program that recognises
 * them finds what it expects; KRFFS has no assigned number and reports its
 * own name. */
#define STATFS_MAGIC_KRFFS 0x4B524646u /* "KRFF" */

#define STATFS_MAGIC_PROC 0x9FA0u

#define STATFS_MAGIC_DEVFS 0x1373u
#define STATFS_MAGIC_SYSFS 0x62656572u

#define STATFS_MAGIC_PIPEFS 0x50495045u
#define STATFS_MAGIC_SOCKFS 0x534F434Bu
#define STATFS_MAGIC_NSFS   0x6E736673u
#define STATFS_MAGIC_ANON   0x09041934u

/**
 * Copy out to `out` the statfs of the KRFFS boot root, of a synthetic
 * mount, or of the file system of a pipe, a socket or an anonymous
 * descriptor, by its `magic`; 0, or -EFAULT. A mounted volume answers from
 * its own statfs (`statfs_fill_mount`).
 *
 * Only the boot root has blocks: the size of its region and the usable
 * bytes of its free nodes, in units of STATFS_BSIZE. Every other magic
 * reports no blocks, which is what Linux reports for /proc, /dev and /sys
 * and for pipefs, sockfs, nsfs and the anonymous inode file system.
 * `f_files` and `f_ffree` are 0, which a reader takes as not counted, since
 * KRFFS keeps no count of inodes, and `f_namelen` is KRFFS's longest name
 * for all of them.
 */
static int32_t statfs_fill(struct user_statfs __user *out, uint32_t magic)
{
    struct user_statfs st;
    uint32_t total = krffs_total_bytes();
    uint32_t free_bytes = krffs_free_bytes();
    int i;

    k_memset(&st, 0, sizeof(st));
    st.f_type = magic;
    st.f_bsize = STATFS_BSIZE;
    st.f_frsize = STATFS_BSIZE;
    if (magic == STATFS_MAGIC_KRFFS) {
        st.f_blocks = total / STATFS_BSIZE;
        st.f_bfree = free_bytes / STATFS_BSIZE;
        st.f_bavail = st.f_bfree;
    }
    st.f_files = 0;
    st.f_ffree = 0;
    st.f_namelen = KRFFS_FILE_NAME_BUFFER_SIZE - 1;
    for (i = 0; i < 4; i++) {
        st.f_spare[i] = 0;
    }
    if (copy_to_user(out, &st, sizeof(st)) != 0) {
        return -EFAULT;
    }
    return 0;
}

/** Copy out to `out` the statfs a file system type gave in `vs`, with the
 * free blocks only a privileged writer takes left out of `f_bavail`; 0, or
 * -EFAULT. */
static int32_t statfs_put(struct user_statfs __user *out, const struct vfs_statfs *vs)
{
    struct user_statfs st;

    k_memset(&st, 0, sizeof(st));
    st.f_type = vs->magic;
    st.f_bsize = vs->bsize;
    st.f_frsize = vs->bsize;
    st.f_blocks = vs->blocks;
    st.f_bfree = vs->bfree;
    st.f_bavail = vs->bfree > vs->reserved ? vs->bfree - vs->reserved : 0;
    st.f_files = vs->files;
    st.f_ffree = vs->ffree;
    st.f_namelen = vs->namelen;
    if (copy_to_user(out, &st, sizeof(st)) != 0) {
        return -EFAULT;
    }
    return 0;
}

/** Copy out to `out` the statfs of the mounted volume `m`, from its type's
 * statfs (`statfs_put`); 0, the type's error, or -EFAULT. */
static int32_t statfs_fill_mount(struct user_statfs __user *out, struct vfs_mount *m)
{
    struct vfs_statfs vs;
    int rc = vfs_statfs(m, &vs);

    if (rc != 0) {
        return rc;
    }
    return statfs_put(out, &vs);
}

/** The magic of the mount a synthetic node of `kind` belongs to: /dev's,
 * /sys's or /proc's, or KRFFS's for VNODE_KRFFS. */
static uint32_t statfs_magic_for_kind(int kind)
{
    if (kind == VNODE_DEV_DIR || kind == VNODE_DEV || kind == VNODE_PTS_DIR || kind == VNODE_PTS ||
        kind == VNODE_DEV_LINK || kind == VNODE_DEV_SUBDIR || kind == VNODE_DEV_MADE) {
        return STATFS_MAGIC_DEVFS;
    }
    if (kind == VNODE_KRFFS) {
        return STATFS_MAGIC_KRFFS;
    }
    if (kind == VNODE_SYS_DIR || kind == VNODE_SYS_LINK || kind == VNODE_SYS_FILE) {
        return STATFS_MAGIC_SYSFS;
    }
    return STATFS_MAGIC_PROC;
}

long ksys_statfs(const char __user *upath, struct user_statfs __user *out)
{
    char *pathbuf;
    int len;
    int dir;

    len = path_from_user_dir(AT_FDCWD, upath, &pathbuf, &dir);
    if (len != 0) {
        return len;
    }
    {
        struct vfs_ref ref;
        const struct vpath *svp;
        uint32_t magic = STATFS_MAGIC_KRFFS;
        int rc = resolve_named(pathbuf, 0, dir, &ref);

        path_buf_put(pathbuf);
        if (rc != 0) {
            return rc;
        }
        svp = vfs_synth_vpath(ref.mnt, ref.node);
        if (svp) {
            magic = statfs_magic_for_kind(svp->kind);
        } else if (!mnt_is_root(ref.mnt)) {
            struct vfs_mount *m = ref.mnt;

            vfs_ref_release(&ref);
            return statfs_fill_mount(out, m);
        }
        vfs_ref_release(&ref);
        return statfs_fill(out, magic);
    }
}

long ksys_fstatfs(int fd, struct user_statfs __user *out)
{
    fd_entry_t *e;
    const struct open_file *ofd;
    struct vfs_statfs vs;

    e = fd_get(current_proc->fds, fd);
    if (!e) {
        return -EBADF;
    }
    ofd = e->ofd;
    switch (e->type) {
    case FD_TYPE_FILE:
    case FD_TYPE_DIR:
    case FD_TYPE_PIPE:
        if (ofd && ofd->vmnt && !mnt_is_root(ofd->vmnt)) {
            return statfs_fill_mount(out, ofd->vmnt);
        }
        if (e->type == FD_TYPE_PIPE && !(ofd && ofd->vmnt)) {
            return statfs_fill(out, STATFS_MAGIC_PIPEFS);
        }
        return statfs_fill(out, STATFS_MAGIC_KRFFS);
    case FD_TYPE_PROC:
        return statfs_fill(out, procfs_fd_is_sys(e) ? STATFS_MAGIC_SYSFS : STATFS_MAGIC_PROC);
    case FD_TYPE_SYNTH_DIR: {
        const struct synth_dir *sd = (const struct synth_dir *)e->priv;

        return statfs_fill(out, sd ? statfs_magic_for_kind(sd->kind) : STATFS_MAGIC_DEVFS);
    }
    case FD_TYPE_SYNTH_PATH: {
        const struct vpath *vp = (const struct vpath *)e->priv;

        return statfs_fill(out, vp ? statfs_magic_for_kind(vp->kind) : STATFS_MAGIC_PROC);
    }
    case FD_TYPE_SOCKET:
    case FD_TYPE_UNIX:
        return statfs_fill(out, STATFS_MAGIC_SOCKFS);
    case FD_TYPE_ANON:
        if (mq_fd_statfs(e, &vs)) {
            return statfs_put(out, &vs);
        }
        return statfs_fill(out, nsfs_ns(e) ? STATFS_MAGIC_NSFS : STATFS_MAGIC_ANON);
    case FD_TYPE_CONSOLE:
    case FD_TYPE_FRAMEBUFFER:
    case FD_TYPE_MOUSE:
    case FD_TYPE_DEVICE:
    case FD_TYPE_PTM:
    case FD_TYPE_PTS:
        return statfs_fill(out, STATFS_MAGIC_DEVFS);
    case FD_TYPE_NONE:
    default:
        return -EBADF;
    }
}

/**
 * statfs(2) as a 64-bit architecture has it: the path and the buffer, the
 * architecture's struct statfs.
 */
long sys_statfs(isr_params_t *params)
{
    if (!current_proc || !SC_ARG(params, 0) || !SC_ARG(params, 1)) {
        return -EFAULT;
    }
    return ksys_statfs((const char __user *)SC_ARG(params, 0),
                       (struct user_statfs __user *)SC_ARG(params, 1));
}

/** fstatfs(2) as a 64-bit architecture has it: the descriptor and the
 * buffer. */
long sys_fstatfs(isr_params_t *params)
{
    if (!current_proc || !SC_ARG(params, 1)) {
        return -EFAULT;
    }
    return ksys_fstatfs((int)SC_ARG(params, 0), (struct user_statfs __user *)SC_ARG(params, 1));
}

/**
 * statfs64(2) of a 32-bit architecture: the path, the caller's
 * sizeof(struct statfs64) and the buffer. The size is checked, and any
 * other is -EINVAL: a caller built against a different layout would
 * otherwise be handed bytes it would read as different fields.
 */
long sys_statfs64(isr_params_t *params)
{
    if (!current_proc || !SC_ARG(params, 0) || !SC_ARG(params, 2)) {
        return -EFAULT;
    }
    if (SC_ARG(params, 1) != sizeof(struct user_statfs)) {
        return -EINVAL;
    }
    return ksys_statfs((const char __user *)SC_ARG(params, 0),
                       (struct user_statfs __user *)SC_ARG(params, 2));
}

/** fstatfs64(2) of a 32-bit architecture: the descriptor, the caller's size
 * and the buffer; a size other than statfs64's is -EINVAL. */
long sys_fstatfs64(isr_params_t *params)
{
    if (SC_ARG(params, 1) != sizeof(struct user_statfs)) {
        return -EINVAL;
    }
    if (!current_proc || !SC_ARG(params, 2)) {
        return -EFAULT;
    }
    return ksys_fstatfs((int)SC_ARG(params, 0), (struct user_statfs __user *)SC_ARG(params, 2));
}

int ksys_ustat_counts(uint32_t dev, uint64_t *bfree, uint64_t *ffree)
{
    struct vfs_statfs vs;
    int rc;
    int i;

    for (i = 0; i < vfs_mount_slots(); i++) {
        struct vfs_mount *m = vfs_mount_at(i);

        if (!m || vfs_mount_base(m) != m || procfs_mount_dev(i, m) != dev) {
            continue;
        }
        *bfree = 0;
        *ffree = 0;
        if (mnt_is_root(m)) {
            *bfree = krffs_free_bytes() / STATFS_BSIZE;
            return 0;
        }
        if (!m->type->ops->statfs) {
            return 0;
        }
        if (vfs_enter(m) != 0) {
            continue;
        }
        rc = vfs_statfs(m, &vs);
        vfs_leave(m);
        if (rc != 0) {
            return rc;
        }
        *bfree = vs.bfree;
        *ffree = vs.ffree;
        return 0;
    }
    return -EINVAL;
}

/* The struct ustat of the kernel's own ABI: 20 bytes on i686, 32 on x86-64,
 * whose f_tinode is a long. */
struct user_ustat {
    int32_t f_tfree;
    unsigned long f_tinode;
    char f_fname[6];
    char f_fpack[6];
};

/**
 * ustat(2): the device number and the buffer. The free blocks and inodes
 * of the file system on that device, truncated to the structure's fields
 * as Linux truncates them, and empty names; -EINVAL for a device with no
 * mounted file system (`ksys_ustat_counts`), -EFAULT for the buffer.
 */
long sys_ustat(isr_params_t *params)
{
    struct user_ustat u;
    uint64_t bfree;
    uint64_t ffree;
    int rc = ksys_ustat_counts((uint32_t)SC_ARG(params, 0), &bfree, &ffree);

    if (rc != 0) {
        return rc;
    }
    k_memset(&u, 0, sizeof(u));
    u.f_tfree = (int32_t)bfree;
    u.f_tinode = (unsigned long)ffree;
    return copy_to_user((void __user *)SC_ARG(params, 1), &u, sizeof(u)) != 0 ? -EFAULT : 0;
}

/* The longest type name sysfs(2) can match; a longer one matches none. */
#define SYSFS_TYPE_NAME_MAX 31

/**
 * sysfs(2): option 1 gives the index of the type named by the string at
 * the second argument, option 2 copies the name of the type whose index is
 * the second argument into the buffer at the third, and option 3 gives the
 * number of types, each in the order /proc/filesystems lists them
 * (`procfs_filesystem_at`). -EINVAL for any other option, an unknown name
 * or an index past the last; a name is read as a path is, so an empty one
 * is -ENOENT and one with no NUL in 4096 bytes -ENAMETOOLONG. The index is
 * an unsigned int, as Linux's `fs_name` takes it.
 */
long sys_sysfs(isr_params_t *params)
{
    char want[SYSFS_TYPE_NAME_MAX + 1];
    const char *name;
    int len;
    int i;

    switch ((int)SC_ARG(params, 0)) {
    case 1:
        len = strnlen_user((const char __user *)SC_ARG(params, 1), VFS_PATH_MAX);
        if (len < 0) {
            return len;
        }
        if (len == 0) {
            return -ENOENT;
        }
        if (len > SYSFS_TYPE_NAME_MAX) {
            return -EINVAL;
        }
        len = strncpy_from_user(want, (const char __user *)SC_ARG(params, 1), sizeof(want));
        if (len < 0) {
            return len == -ENAMETOOLONG ? -EINVAL : len;
        }
        for (i = 0; procfs_filesystem_at(i, &name) >= 0; i++) {
            if (name_is(name, want)) {
                return i;
            }
        }
        return -EINVAL;
    case 2:
        if ((uint32_t)SC_ARG(params, 1) > 0x7FFFFFFFu ||
            procfs_filesystem_at((int)SC_ARG(params, 1), &name) < 0) {
            return -EINVAL;
        }
        return copy_to_user((void __user *)SC_ARG(params, 2), name, k_strlen(name) + 1) != 0
                   ? -EFAULT
                   : 0;
    case 3:
        for (i = 0; procfs_filesystem_at(i, &name) >= 0; i++) {
        }
        return i;
    default:
        return -EINVAL;
    }
}

/**
 * Fill `kst`, which has the boot times, with the stat of the anonymous
 * descriptor `entry`, for `fstat_fill`: a message queue's is its file's, a
 * namespace's nsfs's file of the namespace, a dma-buf's size and distinct
 * inode, and any other's Linux's anonymous inode. Returns 0, or the error
 * of the queue's getattr.
 */
static int32_t fstat_anon(fd_entry_t *entry, struct kstat *kst)
{
    struct vfs_ref mq;

    if (mq_fd_ref(entry, &mq)) {
        return vfs_fill_stat64(kst, &mq);
    }
    kst->st_nlink = 1;
    kst->st_blksize = 4096;
    if (is_dma_buf_file(entry)) {
        struct dma_buf *buf = entry->priv;
        kst->st_mode = S_IFREG | 0600;
        kst->st_ino = buf->ino;
        kst->st_size = (int64_t)buf->size;
    } else if (nsfs_ns(entry)) {
        kst->st_dev = NSFS_DEV;
        kst->st_mode = S_IFREG | 0444;
        kst->st_ino = nsfs_ns(entry)->inum;
    } else {
        kst->st_mode = 0600;
    }
    return 0;
}

/**
 * Fill `kst`, zeroed with the boot times, for the synthetic descriptor
 * `entry`, FD_TYPE_SYNTH_DIR or FD_TYPE_SYNTH_PATH, as `fstat_fill_attrs`
 * describes, with the owner its record keeps (`procfs_owner_keep`) when the
 * node's inode keeps one.
 */
static void fstat_synth(fd_entry_t *entry, struct kstat *kst)
{
    const struct synth_owner *own = NULL;

    if (entry->type == FD_TYPE_SYNTH_DIR) {
        const struct synth_dir *sd = (const struct synth_dir *)entry->priv;
        struct vpath vp;

        own = sd ? &sd->own : NULL;
        synth_dir_vpath(sd, &vp);
        if (synth_stat_node(&vp, kst, 1) != 0) {
            kst->st_mode = own && own->kept ? synth_pid_dir_mode(&vp) : S_IFDIR | 0555;
            kst->st_nlink = 1;
            kst->st_blksize = 4096;
            kst->st_dev = PROCFS_FSID;
            if (synth_is_sys(&vp)) {
                synth_sys_identity(&vp, kst, 1);
            }
        }
    } else {
        const struct synth_path *sp = (const struct synth_path *)entry->priv;
        const struct vpath *vp = sp ? &sp->vp : NULL;

        own = sp ? &sp->own : NULL;
        if (!vp || synth_stat_node(vp, kst, 0) != 0) {
            int dev = vp && (vp->kind == VNODE_DEV || vp->kind == VNODE_PTS);

            k_memset(kst, 0, sizeof(*kst));
            stat64_boot_times(kst);
            kst->st_mode = dev ? 020600 : (S_IFREG | 0444); /* S_IFCHR | 0600 */
            kst->st_blksize = 4096;
            kst->st_dev = dev ? DEVFS_FSID : PROCFS_FSID;
            if (synth_is_sys(vp)) {
                synth_sys_identity(vp, kst, 0);
            }
        }
    }
    if (own && own->kept) {
        kst->st_uid = own->uid;
        kst->st_gid = own->gid;
    }
}

/**
 * Fill `kst` with the attributes of what the descriptor `entry` is open on,
 * for fstat64, for fstatat64 and statx with AT_EMPTY_PATH, and for a
 * followed /proc/PID/fd link; 0, or the error of a mounted type's getattr.
 * Where the object has an identity of its own, a socket, a pipe, a device, a
 * /proc snapshot or a KRFFS node, the inode is that identity, the number the
 * descriptor's /proc/PID/fd link shows, so that a program can match the two.
 * An object without times of its own reports the boot time.
 *
 * A file or a directory of a mounted type is `vfs_fill_stat64`'s. The KRFFS
 * root directory has the identity a stat of "/" gives, inode 1, mode 0755
 * and the root's own times. A KRFFS node reports its attributes while
 * `fd_node_check` finds it live; a descriptor whose file went away by a
 * route that did not look for its holders reports only the boot times, and
 * reads nothing of a node that may be the free list's now.
 *
 * A network socket is a socket of mode 0666 whose inode is the socket's
 * own, the number /proc/net/tcp shows, and an AF_UNIX socket one of mode
 * 0777. A namespace descriptor is nsfs's file of the namespace, whose inode
 * is the namespace's, and a message queue's descriptor the queue's file in
 * the mqueue file system (`mq_fd_ref`), which a stat of /dev/mqueue/NAME
 * gives; any other anonymous descriptor, an eventfd, an epoll or a timerfd
 * among them, is Linux's anonymous inode, mode 0600 with no type bits
 * (`fstat_anon`). A pipe end is a FIFO of mode 0666 with the pipe's own
 * inode.
 *
 * A devfs device reports the identity a stat of its /dev path gives, the
 * mode included, and the console that of /dev/console, which ttyname
 * compares. A /proc file is a regular file of mode 0444 whose inode is that
 * of the path it was opened by; a file of a process, or an fdinfo file, is
 * owned as the last lookup of its path found it (`procfs_fd_owner`), the
 * open's own or a later stat's, as Linux's inode keeps the owner its
 * lookup set, and any other is root's; oom_score_adj, the user
 * namespace's maps and a file a privileged caller may write have mode 0644,
 * and a /sys file is on sysfs with a size of 4096.
 *
 * A /dev, /proc or /sys directory reports what a stat of its path gives,
 * from the descriptor's record of its kind, process and thread, a process's
 * fd directory the number of descriptors open as its size, or a bare
 * directory once its process has exited, which for an fd directory only the
 * callers of `fstat_fill` see, since `fstat_getattr` fails then. An O_PATH
 * descriptor on a synthetic leaf, or on a /proc link under O_NOFOLLOW,
 * reports what an lstat of its path gives while the node is there, and a
 * bare device or file once it is gone. Either kind of descriptor on a
 * process's fd, fdinfo or task directory, a file of /proc/<pid>, an fdinfo
 * file or a process's link reports the owner the last lookup of its node gave
 * (`procfs_owner_keep`), as Linux's fstat reads the inode's owner
 * (fs/stat.c:229), which only a lookup sets. A pty slave reports what a stat
 * of /dev/pts/N gives, which the /proc/self/fd link names, or a character
 * device of mode 0620 once its pty is gone. Anything else, a pty master, the
 * framebuffer or the mouse, is a character device of mode 0666 and size 0.
 */
static int32_t fstat_fill_attrs(fd_entry_t *entry, struct kstat *kst)
{
    k_memset(kst, 0, sizeof(*kst));
    stat64_boot_times(kst);
    if ((entry->type == FD_TYPE_FILE || entry->type == FD_TYPE_DIR) && entry->ofd &&
        entry->ofd->vmnt && !mnt_is_root(entry->ofd->vmnt)) {
        struct vfs_ref r;

        r.mnt = entry->ofd->vmnt;
        r.node = entry->ofd->vnode;
        return vfs_fill_stat64(kst, &r);
    }
    if (entry->type == FD_TYPE_DIR && entry->node == NULL) {
        kst->st_mode = S_IFDIR | 0755;
        kst->st_ino = 1;
        kst->st_nlink = 1;
        kst->st_blksize = 4096;
        stat64_root_times(kst);
    } else if (entry->node) {
        if (fd_node_check(entry) == 0) {
            krffs_fill_stat64(kst, entry->node);
        }
    } else if (entry->type == FD_TYPE_SOCKET) {
        kst->st_mode = 0140666; /* S_IFSOCK | 0666 */
        kst->st_nlink = 1;
        kst->st_blksize = 4096;
        kst->st_ino = entry->sock ? entry->sock->inode : 0u;
    } else if (entry->type == FD_TYPE_UNIX) {
        kst->st_mode = 0140777; /* S_IFSOCK | 0777 */
        kst->st_nlink = 1;
        kst->st_blksize = 4096;
        kst->st_ino = entry->priv ? unix_inode(entry) : 0u;
    } else if (entry->type == FD_TYPE_ANON) {
        return fstat_anon(entry, kst);
    } else if (entry->type == FD_TYPE_PIPE) {
        pipe_t *pp = (pipe_t *)entry->priv;
        kst->st_mode = 010666; /* S_IFIFO | 0666 */
        kst->st_nlink = 1;
        kst->st_blksize = 4096;
        kst->st_ino = pp ? pp->inode : 0u;
    } else if (entry->type == FD_TYPE_DEVICE) {
        kst->st_mode = devfs_fd_mode(entry);
        devfs_fd_owner(entry, &kst->st_uid, &kst->st_gid);
        kst->st_nlink = 1;
        kst->st_blksize = 4096;
        kst->st_dev = DEVFS_FSID;
        kst->st_rdev = devfs_rdev(entry);
        kst->st_ino = devfs_fd_ino(entry);
    } else if (entry->type == FD_TYPE_PROC) {
        uint32_t uid, gid;
        int owned, perm;

        kst->st_mode = 0100444; /* S_IFREG | 0444 */
        kst->st_nlink = 1;
        kst->st_blksize = 4096;
        kst->st_dev = procfs_fd_is_sys(entry) ? SYSFS_FSID : PROCFS_FSID;
        kst->st_size = procfs_fd_is_sys(entry) ? 4096 : 0;
        kst->st_ino = procfs_fd_ino(entry);
        owned = procfs_fd_owner(entry, &uid, &gid);
        if (owned >= 0) {
            kst->st_uid = uid;
            kst->st_gid = gid;
        }
        perm = procfs_fd_writable(entry);
        if (perm != 0) {
            kst->st_mode = 0100000 | (uint32_t)perm; /* S_IFREG */
        } else if (owned == 1) {
            kst->st_mode = 0100644; /* S_IFREG | 0644 */
        }
    } else if (entry->type == FD_TYPE_CONSOLE) {
        uint32_t cmode = 0, crdev = 0, cino = 0;
        (void)devfs_stat("console", &cmode, &crdev, &cino);
        kst->st_mode = 020666; /* S_IFCHR | 0666 */
        kst->st_nlink = 1;
        kst->st_blksize = 4096;
        kst->st_dev = DEVFS_FSID;
        kst->st_rdev = crdev;
        kst->st_ino = cino;
    } else if (entry->type == FD_TYPE_SYNTH_DIR || entry->type == FD_TYPE_SYNTH_PATH) {
        fstat_synth(entry, kst);
    } else if (entry->type == FD_TYPE_PTS) {
        struct vpath vp;
        k_memset(&vp, 0, sizeof(vp));
        vp.kind = VNODE_PTS;
        vp.pts_index = pty_slave_index(entry);
        if (synth_stat_node(&vp, kst, 1) != 0) {
            kst->st_mode = 020620; /* S_IFCHR | 0620 */
            kst->st_nlink = 1;
            kst->st_blksize = 4096;
            kst->st_dev = DEVFS_FSID;
        }
    } else {
        kst->st_mode = 020666; /* S_IFCHR | 0666 */
        kst->st_nlink = 1;
        kst->st_blksize = 4096;
    }
    return 0;
}

/**
 * Fill `kst` as `fstat_fill_attrs` does, with the id of the mount the
 * object is on: the description's mount for a file or a directory of a
 * mounted type, the root's for the KRFFS root and its nodes, the /dev,
 * /proc or /sys mount for a synthetic node by its device, and none for a
 * pipe, a socket or an anonymous descriptor, which Linux keeps on internal
 * mounts that no mountinfo lists.
 */
static int32_t fstat_fill(fd_entry_t *entry, struct kstat *kst)
{
    int32_t rc = fstat_fill_attrs(entry, kst);

    if (rc != 0) {
        return rc;
    }
    if ((entry->type == FD_TYPE_FILE || entry->type == FD_TYPE_DIR) && entry->ofd &&
        entry->ofd->vmnt) {
        kst->st_mnt_id = procfs_mount_id(entry->ofd->vmnt);
    } else if (entry->node || (entry->type == FD_TYPE_DIR && entry->node == NULL)) {
        kst->st_mnt_id = procfs_mount_id(vfs_root_mount());
    } else {
        kst->st_mnt_id = procfs_synth_mount_id(kst->st_dev, NULL);
    }
    return 0;
}

/**
 * Fill `kst` for a stat of the descriptor `entry`, fstat64's, fstatat64's
 * and statx's with AT_EMPTY_PATH and a followed /proc/PID/fd link's, as
 * `fstat_fill` does; but -ENOENT for a descriptor on a process's fd
 * directory once the process is gone, as Linux's `proc_fd_getattr` fails
 * when `proc_readfd_count` finds no task (fs/proc/fd.c:291-292, 362-364).
 * Access checks, sync_file_range and cachestat read the inode without its
 * getattr on Linux, and call `fstat_fill` directly.
 */
static int32_t fstat_getattr(fd_entry_t *entry, struct kstat *kst)
{
    if (entry->type == FD_TYPE_SYNTH_DIR || entry->type == FD_TYPE_SYNTH_PATH) {
        struct vpath vp;

        if (entry->type == FD_TYPE_SYNTH_DIR) {
            synth_dir_vpath((const struct synth_dir *)entry->priv, &vp);
        } else if (entry->priv) {
            vp = ((const struct synth_path *)entry->priv)->vp;
        } else {
            vp.kind = VNODE_ENOENT;
        }
        if (vp.kind == VNODE_PROC_FD_DIR && procfs_resolve_id(&vp) < 0) {
            return -ENOENT;
        }
    }
    return fstat_fill(entry, kst);
}

/**
 * Validate sync_file_range's descriptor, range and inode kind, in Linux's
 * order, then flush the file's data for WRITE (bit 1) as fdatasync does
 * (`sync_entry`). All writes here finish synchronously, so the WAIT_BEFORE
 * and WAIT_AFTER bits need no separate wait. Returns 0, -EBADF for an
 * absent or O_PATH descriptor, -EINVAL for flags outside 0..7 or a
 * negative or overflowing range, -ESPIPE for an inode other than a regular
 * file, directory, block device or symlink, or a flush error.
 */
long ksys_sync_file_range(int fd, int64_t off, int64_t len, uint32_t flags)
{
    fd_entry_t *entry;
    struct kstat st;
    uint32_t kind;
    long rc;

    if (!current_proc) {
        return -ESRCH;
    }
    entry = fd_get(current_proc->fds, fd);
    if (!entry || (entry->flags & FD_FLAG_PATH)) {
        return -EBADF;
    }
    if ((flags & ~7u) || off < 0 || len < 0 || off > 0x7FFFFFFFFFFFFFFFll - len) {
        return -EINVAL;
    }
    rc = fstat_fill(entry, &st);
    if (rc != 0) {
        return rc;
    }
    kind = st.st_mode & S_IFMT;
    if (kind != S_IFREG && kind != S_IFDIR && kind != S_IFBLK && kind != 0120000u) {
        return -ESPIPE;
    }
    if (!(flags & 2u)) {
        return 0;
    }
    return sync_entry(entry, vfs_fdatasync);
}

/** sync_file_range(2) with one register each for its 64-bit range. */
long sys_sync_file_range(isr_params_t *params)
{
    return ksys_sync_file_range((int)SC_ARG(params, 0), (int64_t)SC_ARG(params, 1),
                                (int64_t)SC_ARG(params, 2), (uint32_t)SC_ARG(params, 3));
}

/**
 * fstat64(2), and fstat(2) on x86-64: copy out the attributes of what the
 * descriptor in argument 0 is open on (`fstat_fill`), an O_PATH
 * descriptor's included, to the user's buffer in argument 1, with the
 * owner and group the caller's user namespace sees; 0, -EBADF for a
 * descriptor that is not open, -EFAULT for a NULL or bad buffer, or the
 * error of a mounted type's getattr. The call holds the big lock unless the
 * descriptor's type runs without it (`fd_call_lock`); i386's fstat calls
 * come here too (arch/x86_64/ia32_fs.c).
 */
long sys_fstat64(isr_params_t *params)
{
    int32_t rc;
    int fd = (int)SC_ARG(params, 0);
    struct user_stat __user *st = (struct user_stat __user *)SC_ARG(params, 1);
    struct kstat kst;
    fd_entry_t *entry;
    int big;

    if (!current_proc || !st) {
        return -EFAULT;
    }
    entry = fd_get(current_proc->fds, fd);
    if (!entry) {
        return -EBADF;
    }
    big = file_read_lock(entry);
    rc = fstat_getattr(entry, &kst);
    file_ops_unlock(big);
    if (rc != 0) {
        return rc;
    }
    return stat64_put(st, &kst);
}

/**
 * Whether the user's path is the empty string, which with AT_EMPTY_PATH names
 * the descriptor a call is given rather than a file below it. A NULL path
 * counts as empty, as it does on Linux since 6.11. Returns 1, 0 or -EFAULT.
 */
static int user_path_empty(const char __user *upath)
{
    char c;

    if (!upath) {
        return 1;
    }
    if (copy_from_user(&c, upath, 1) != 0) {
        return -EFAULT;
    }
    return c == '\0';
}

/**
 * Fill `kst` for the absolute canonical path `path`, a final symbolic link
 * followed when `follow`: the path half of fstatat64. The KRFFS root has the
 * fixed identity `stat` reports for it, and a /proc link is reported as a
 * link when it is not followed. The walk keeps the lock of the mount it ends
 * on until the attributes are read, as `do_stat64`'s does, and for the same
 * reason.
 */
static int32_t fstatat_path(const char *path, int follow, struct kstat *kst)
{
    struct vfs_ref ref;
    const struct vpath *svp;
    int32_t rc;

    k_memset(kst, 0, sizeof(*kst));
    if (names_tree_root(path) && vfs_root_mount()->type == &vfs_krffs_type) {
        kst->st_mode = S_IFDIR | 0755;
        kst->st_ino = 1;
        kst->st_nlink = 1;
        kst->st_blksize = 4096;
        stat64_root_times(kst);
        kst->st_mnt_id = procfs_mount_id(vfs_root_mount());
        return 0;
    }
    rc = vfs_resolve(0, path, VFS_WALK_LOCK | (follow ? 0 : VFS_WALK_NOFOLLOW), &ref, 0, 0);
    if (rc != 0) {
        return rc;
    }
    svp = vfs_synth_vpath(ref.mnt, ref.node);
    if (svp) {
        /* A followed /proc/PID/fd link has its file's mount already. */
        rc = synth_stat(svp, kst, follow);
        if (rc == 0 && kst->st_mnt_id == 0) {
            kst->st_mnt_id = procfs_synth_mount_id(kst->st_dev, ref.mnt);
        }
    } else if (!mnt_is_root(ref.mnt)) {
        rc = vfs_fill_stat64(kst, &ref);
        kst->st_mnt_id = procfs_mount_id(ref.mnt);
    } else {
        krffs_fill_stat64(kst, (krffs_node_t *)ref.node);
        kst->st_mnt_id = procfs_mount_id(ref.mnt);
    }
    vfs_walk_unlock(ref.mnt);
    vfs_ref_release(&ref);
    return rc;
}

/**
 * The attributes fstatat64 and statx report for `upath` and `flags`, into
 * `kst`, whose flags the caller has checked.
 *
 * The path resolves as openat's does: absolute, or relative to the working
 * directory for AT_FDCWD or to the directory `dirfd` is open on (see
 * copy_path_at). AT_SYMLINK_NOFOLLOW reports a final symbolic link itself,
 * but not one the path names as a directory by a trailing slash, "." or
 * "..", which is followed, as Linux's walk does; a path that names a
 * directory by its end and reaches anything else is -ENOTDIR.
 * With AT_EMPTY_PATH an empty or null path reports the object `dirfd` refers
 * to, whatever it is, as fstat64 does, or the working directory for
 * AT_FDCWD. Returns 0 or a negative errno.
 */
static int32_t stat_at(int dirfd, const char __user *upath, int flags, struct kstat *kst)
{
    int follow = !(flags & AT_SYMLINK_NOFOLLOW);
    char *pathbuf;
    int empty = 0;
    int dir = 0;
    int32_t rc;

    if (flags & AT_EMPTY_PATH) {
        empty = user_path_empty(upath);
        if (empty < 0) {
            return empty;
        }
    }
    if (empty && dirfd != AT_FDCWD) {
        fd_entry_t *entry = fd_get(current_proc->fds, dirfd);

        return entry ? fstat_getattr(entry, kst) : -EBADF;
    }
    if (empty) {
        pathbuf = path_buf_get();
        if (!pathbuf) {
            return -ENOMEM;
        }
        k_strncpy(pathbuf, proc_cwd(current_proc), VFS_PATH_MAX);
    } else {
        if (!upath) {
            return -EFAULT;
        }
        rc = path_from_user_dir(dirfd, upath, &pathbuf, &dir);
        if (rc != 0) {
            return rc;
        }
    }
    rc = fstatat_path(pathbuf, follow || dir, kst);
    path_buf_put(pathbuf);
    if (rc == 0 && dir && (kst->st_mode & S_IFMT) != S_IFDIR) {
        return -ENOTDIR;
    }
    return rc;
}

/**
 * `stat_at` for a caller without the big lock: a descriptor with
 * AT_EMPTY_PATH that `file_vfs_unlocked` admits, or a path the name cache
 * finishes (`vfs_resolve_cached`), absolute or relative to the working
 * directory (`path_from_user_unlocked`), as Linux's `vfs_statx` walks a path
 * under RCU first (fs/namei.c:2651-2653). 0, a negative errno, or
 * CALL_LOCKED with nothing done, for `stat_at` under the big lock.
 */
static int32_t stat_at_unlocked(int dirfd, const char __user *upath, int flags, struct kstat *kst)
{
    int follow = !(flags & AT_SYMLINK_NOFOLLOW);
    struct vfs_ref ref;
    char *pathbuf;
    int empty = 0, dir = 0;
    int32_t rc;

    if (flags & AT_EMPTY_PATH) {
        empty = user_path_empty(upath);
        if (empty < 0) {
            return empty;
        }
    }
    if (empty) {
        fd_entry_t *entry = dirfd == AT_FDCWD ? NULL : fd_get(current_proc->fds, dirfd);

        if (!entry || !file_vfs_unlocked(entry)) {
            return CALL_LOCKED;
        }
        return fstat_getattr(entry, kst);
    }
    if (!upath) {
        return -EFAULT;
    }
    rc = path_from_user_unlocked(dirfd, upath, &pathbuf, &dir);
    if (rc != 0) {
        return rc;
    }
    rc = vfs_resolve_cached(pathbuf, VFS_WALK_LOCK | (follow || dir ? 0 : VFS_WALK_NOFOLLOW), &ref,
                            NULL);
    path_buf_put(pathbuf);
    if (rc != 0) {
        return rc == VFS_CACHED_MISS ? CALL_LOCKED : rc;
    }
    rc = vfs_fill_stat64(kst, &ref);
    kst->st_mnt_id = procfs_mount_id(ref.mnt);
    vfs_walk_unlock(ref.mnt);
    vfs_ref_release(&ref);
    if (rc == 0 && dir && (kst->st_mode & S_IFMT) != S_IFDIR) {
        return -ENOTDIR;
    }
    return rc;
}

/**
 * fstatat64(2), and newfstatat(2) on x86-64: copy out the attributes
 * `stat_at` finds for the directory descriptor, the path and the flags of
 * arguments 0, 1 and 3 to the user's buffer in argument 2, with the owner
 * and group the caller's user namespace sees; 0 or a negative errno.
 * AT_NO_AUTOMOUNT is accepted and means nothing here, and any other flag
 * but AT_SYMLINK_NOFOLLOW and AT_EMPTY_PATH is -EINVAL before the path is
 * read. GNU libc's fstat is this call with AT_EMPTY_PATH, on i386 when
 * statx is missing. The call runs without the big lock, which it takes
 * for what `stat_at_unlocked` leaves.
 */
long sys_fstatat64(isr_params_t *params)
{
    struct user_stat __user *st = (struct user_stat __user *)SC_ARG(params, 2);
    int flags = (int)SC_ARG(params, 3);
    struct kstat kst;
    int32_t rc;

    if (!current_proc || !st) {
        return -EFAULT;
    }
    if (flags & ~(AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH | AT_NO_AUTOMOUNT)) {
        return -EINVAL;
    }
    rc = bkl_held() ? CALL_LOCKED
                    : stat_at_unlocked((int)SC_ARG(params, 0),
                                       (const char __user *)SC_ARG(params, 1), flags, &kst);
    if (rc == CALL_LOCKED) {
        int big = !bkl_held();

        if (big) {
            bkl_enter();
        }
        rc = stat_at((int)SC_ARG(params, 0), (const char __user *)SC_ARG(params, 1), flags, &kst);
        if (big) {
            bkl_exit();
        }
    }
    return rc != 0 ? rc : stat64_put(st, &kst);
}

/* The statx flags that ask how fresh an answer must be; this kernel has one
 * answer, and both at once are EINVAL. */
#define AT_STATX_SYNC_TYPE 0x6000

/* The attributes statx reports, the mount id among them when the file is
 * on a mount of the table, and the mask bit reserved for a larger
 * structure, which is EINVAL. */
#define STATX_BASIC_STATS 0x7FFu
#define STATX_MNT_ID      0x1000u
#define STATX_RESERVED    0x80000000u

/* struct statx as Linux writes it: 256 bytes, laid out the same on every
 * architecture, 64-bit fields at multiples of eight. */
struct linux_statx_time {
    int64_t tv_sec;
    uint32_t tv_nsec;
    int32_t reserved;
};

struct linux_statx {
    uint32_t stx_mask;
    uint32_t stx_blksize;
    uint64_t stx_attributes;
    uint32_t stx_nlink;
    uint32_t stx_uid;
    uint32_t stx_gid;
    uint16_t stx_mode;
    uint16_t spare0;
    uint64_t stx_ino;
    uint64_t stx_size;
    uint64_t stx_blocks;
    uint64_t stx_attributes_mask;
    struct linux_statx_time stx_atime;
    struct linux_statx_time stx_btime;
    struct linux_statx_time stx_ctime;
    struct linux_statx_time stx_mtime;
    uint32_t stx_rdev_major;
    uint32_t stx_rdev_minor;
    uint32_t stx_dev_major;
    uint32_t stx_dev_minor;
    uint64_t stx_mnt_id;
    uint64_t spare[13];
};

typedef char linux_statx_is_256_bytes[sizeof(struct linux_statx) == 256 ? 1 : -1];

/** The major number of the device number `dev` in Linux's 32-bit
 * encoding, which st_dev and st_rdev have: bits 8 to 19. */
static uint32_t dev_major(uint64_t dev)
{
    return (uint32_t)(dev >> 8) & 0xFFFu;
}

/** The minor number of `dev` in the same encoding: bits 0 to 7, and bits
 * 20 to 31 above them. */
static uint32_t dev_minor(uint64_t dev)
{
    return ((uint32_t)dev & 0xFFu) | (((uint32_t)dev >> 12) & 0xFFF00u);
}

/**
 * Put the attributes of `st` into `sx` as statx reports them: the basic
 * set, the owner and group the caller's user namespace sees, the device
 * numbers split into their major and minor numbers, the times whole, as
 * the signed 64-bit seconds the file system stores, on i386 as on x86-64,
 * and the mount id with STATX_MNT_ID when the file has one.
 */
static void statx_from_kstat(struct linux_statx *sx, const struct kstat *st)
{
    k_memset(sx, 0, sizeof(*sx));
    sx->stx_mask = STATX_BASIC_STATS;
    sx->stx_blksize = st->st_blksize;
    sx->stx_nlink = st->st_nlink;
    sx->stx_uid = uid_seen(st->st_uid);
    sx->stx_gid = gid_seen(st->st_gid);
    sx->stx_mode = (uint16_t)st->st_mode;
    sx->stx_ino = st->st_ino;
    sx->stx_size = (uint64_t)st->st_size;
    sx->stx_blocks = st->st_blocks;
    sx->stx_atime.tv_sec = st->st_atime;
    sx->stx_atime.tv_nsec = st->st_atime_nsec;
    sx->stx_ctime.tv_sec = st->st_ctime;
    sx->stx_ctime.tv_nsec = st->st_ctime_nsec;
    sx->stx_mtime.tv_sec = st->st_mtime;
    sx->stx_mtime.tv_nsec = st->st_mtime_nsec;
    sx->stx_rdev_major = dev_major(st->st_rdev);
    sx->stx_rdev_minor = dev_minor(st->st_rdev);
    sx->stx_dev_major = dev_major(st->st_dev);
    sx->stx_dev_minor = dev_minor(st->st_dev);
    if (st->st_mnt_id != 0) {
        sx->stx_mask |= STATX_MNT_ID;
        sx->stx_mnt_id = st->st_mnt_id;
    }
}

/**
 * statx takes dirfd, path, flags, mask and struct statx buffer.
 *
 * The attributes are fstatat64's, found by `stat_at` for the same path and
 * flags, in struct statx: the basic set of STATX_BASIC_STATS whatever `mask`
 * asks for, since the kernel may report more than it is asked, and the id of
 * the file's mount, STATX_MNT_ID, as /proc/PID/mountinfo gives it, for a
 * file on a mount of the table, with no birth time and no attributes. The
 * two sync flags are accepted and change nothing; both at once, a flag
 * fstatat64 does not take, or STATX__RESERVED in `mask` is EINVAL. On i386
 * GNU libc and musl call this first for every stat and fall back to the
 * stat64 calls on ENOSYS alone.
 */
long sys_statx(isr_params_t *params)
{
    int flags = (int)SC_ARG(params, 2);
    void __user *out = (void __user *)SC_ARG(params, 4);
    struct kstat kst;
    struct linux_statx sx;
    int32_t rc;

    if (!current_proc) {
        return -EFAULT;
    }
    if ((flags & ~(AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH | AT_NO_AUTOMOUNT | AT_STATX_SYNC_TYPE)) ||
        (flags & AT_STATX_SYNC_TYPE) == AT_STATX_SYNC_TYPE ||
        (SC_ARG(params, 3) & STATX_RESERVED)) {
        return -EINVAL;
    }
    rc = stat_at((int)SC_ARG(params, 0), (const char __user *)SC_ARG(params, 1), flags, &kst);
    if (rc != 0) {
        return rc;
    }
    statx_from_kstat(&sx, &kst);
    return copy_to_user(out, &sx, sizeof(sx)) != 0 ? -EFAULT : 0;
}

/*
 * File handles, name_to_handle_at(2) and open_by_handle_at(2). A handle is
 * Linux's FILEID_INO64_GEN: the file's inode number in 64 bits and a
 * generation of 0, twelve bytes, which identify the file within its mount.
 * The types whose files a handle reopens are those Linux gives export
 * operations, ext2, tmpfs and vfat, and KRFFS; a handle of a file of any
 * other mount is a FID, which AT_HANDLE_FID asks for, and which identifies
 * the file and reopens nothing, as Linux's of a type without them.
 */
#define MAX_HANDLE_SZ     128
#define FILEID_INO64_GEN  0x51
#define HANDLE_BYTES      12u
#define AT_HANDLE_FID     0x200
#define HANDLE_WALK_DEPTH 64

/* struct file_handle's fixed part, which f_handle's bytes follow. */
struct linux_file_handle {
    uint32_t handle_bytes;
    int32_t handle_type;
};

/** Whether the files of the mount `m` reopen from a handle. */
static int handle_decodable(const struct vfs_mount *m)
{
    const struct vfs_type *t = m ? vfs_mount_base(m)->type : NULL;

    return t == &vfs_ext2_type || t == &vfs_tmpfs_type || t == &vfs_vfat_type ||
           t == &vfs_krffs_type || t == &vfs_krffs_volume_type;
}

/**
 * name_to_handle_at takes dirfd, path, the struct file_handle, the int the
 * mount's id goes into, and flags.
 *
 * The file is `stat_at`'s for the same dirfd and path, a final symbolic
 * link reported itself unless AT_SYMLINK_FOLLOW, and the object dirfd
 * refers to with AT_EMPTY_PATH and an empty path. Its handle (above) goes
 * into the struct, and the id /proc/PID/mountinfo gives its mount into the
 * int. A struct whose handle_bytes is below 12 gets 12 there and no handle,
 * and the call answers EOVERFLOW, with the mount's id written, so that the
 * caller can ask again with room, as on Linux.
 *
 * 0, or -EINVAL for a flag other than AT_SYMLINK_FOLLOW, AT_EMPTY_PATH and
 * AT_HANDLE_FID, or a handle_bytes above MAX_HANDLE_SZ; the path's error;
 * -EOPNOTSUPP for a file whose type reopens nothing without AT_HANDLE_FID,
 * and for an object on no mount of the table, a pipe, a socket or an
 * anonymous descriptor; -EOVERFLOW; or -EFAULT.
 */
long sys_name_to_handle_at(isr_params_t *params)
{
    struct linux_file_handle __user *uh = (struct linux_file_handle __user *)SC_ARG(params, 2);
    int32_t __user *umnt = (int32_t __user *)SC_ARG(params, 3);
    int flags = (int)SC_ARG(params, 4);
    struct linux_file_handle h;
    unsigned char bytes[HANDLE_BYTES];
    struct kstat kst;
    int32_t rc, mnt_id;
    uint32_t i;

    if (!current_proc) {
        return -EFAULT;
    }
    if (flags & ~(AT_SYMLINK_FOLLOW | AT_EMPTY_PATH | AT_HANDLE_FID)) {
        return -EINVAL;
    }
    rc = stat_at((int)SC_ARG(params, 0), (const char __user *)SC_ARG(params, 1),
                 ((flags & AT_SYMLINK_FOLLOW) ? 0 : AT_SYMLINK_NOFOLLOW) | (flags & AT_EMPTY_PATH),
                 &kst);
    if (rc != 0) {
        return rc;
    }
    if (kst.st_mnt_id == 0 ||
        (!(flags & AT_HANDLE_FID) && !handle_decodable(vfs_mount_at((int)kst.st_mnt_id - 1)))) {
        return -EOPNOTSUPP;
    }
    if (copy_from_user(&h, uh, sizeof(h)) != 0) {
        return -EFAULT;
    }
    if (h.handle_bytes > MAX_HANDLE_SZ) {
        return -EINVAL;
    }
    rc = h.handle_bytes < HANDLE_BYTES ? -EOVERFLOW : 0;
    h.handle_bytes = HANDLE_BYTES;
    h.handle_type = FILEID_INO64_GEN;
    for (i = 0; i < 8; i++) {
        bytes[i] = (unsigned char)(kst.st_ino >> (8 * i));
    }
    bytes[8] = bytes[9] = bytes[10] = bytes[11] = 0;
    mnt_id = (int32_t)kst.st_mnt_id;
    if (copy_to_user(umnt, &mnt_id, sizeof(mnt_id)) != 0 || copy_to_user(uh, &h, sizeof(h)) != 0 ||
        (rc == 0 && copy_to_user((char __user *)uh + sizeof(h), bytes, sizeof(bytes)) != 0)) {
        return -EFAULT;
    }
    return rc;
}

/* One directory of `handle_find`'s walk: held, open, where its listing is,
 * and the length of its path in the walk's buffer. */
struct handle_level {
    struct vfs_ref dir;
    void *h;
    uint32_t cursor;
    uint32_t len;
};

/** Open the held directory `dir` as level `l` of a walk, its path `len`
 * bytes long; on failure `dir` is released. 0, or the type's error. */
static int32_t handle_push(struct handle_level *l, struct vfs_ref *dir, uint32_t len)
{
    int32_t rc = vfs_open_node(dir->mnt, dir->node, 0, &l->h);

    if (rc != 0) {
        vfs_ref_release(dir);
        return rc;
    }
    l->dir = *dir;
    l->cursor = 0;
    l->len = len;
    return 0;
}

/** Close and release level `l` of a walk. */
static void handle_pop(struct handle_level *l)
{
    vfs_release(l->dir.mnt, l->h);
    vfs_ref_release(&l->dir);
}

/**
 * Take the entry `de` of the walk's level `depth` of `lv`: put its path in
 * `out` after the level's, and give 1 when it has the inode `ino`; else
 * open it as the next level when it is a directory of the mount of `root`
 * that fits, and give 0. `*depth` is then the deeper level's.
 */
static int handle_entry(struct handle_level *lv, int *depth, const struct vfs_dirent *de,
                        const struct vfs_ref *root, uint64_t ino, char *out)
{
    struct handle_level *l = &lv[*depth];
    uint32_t len = (uint32_t)k_strlen(de->name);
    uint32_t n = l->len + (l->len ? 1u : 0u) + len;
    struct vfs_ref dir;

    if ((de->name[0] == '.' && (!de->name[1] || (de->name[1] == '.' && !de->name[2]))) ||
        n + 1 > VFS_PATH_MAX || (de->ino != ino && !de->is_dir)) {
        return 0;
    }
    if (l->len) {
        out[l->len] = '/';
    }
    k_memcpy(out + n - len, de->name, len + 1);
    if (de->ino == ino) {
        return 1;
    }
    if (*depth + 1 < HANDLE_WALK_DEPTH &&
        vfs_resolve(&l->dir, de->name, VFS_WALK_NOFOLLOW, &dir, 0, 0) == 0) {
        if (dir.mnt != root->mnt) {
            vfs_ref_release(&dir);
        } else if (handle_push(&lv[*depth + 1], &dir, n) == 0) {
            (*depth)++;
        }
    }
    return 0;
}

/**
 * Find the entry of inode `ino` on the mount of `root`, its root directory,
 * held: its path below the root, without a leading slash, into `out` of
 * VFS_PATH_MAX bytes, "" for the root itself. The tree is read depth first
 * through the type's readdir, as find(1) reads it, each directory on the way
 * down held and open; a directory that is another mount's place is passed
 * by, as is one past HANDLE_WALK_DEPTH or a path that does not fit. 0, or
 * -ESTALE when no entry has the inode, or -ENOMEM.
 */
static int32_t handle_find(const struct vfs_ref *root, uint64_t ino, char *out)
{
    struct handle_level *lv = kmalloc(sizeof(*lv) * HANDLE_WALK_DEPTH);
    struct vfs_dirent *de = kmalloc(sizeof(*de));
    struct vfs_ref dir;
    struct vfs_stat st;
    int depth;
    int32_t rc = -ESTALE;

    out[0] = '\0';
    if (!lv || !de) {
        rc = -ENOMEM;
    } else if ((vfs_getattr(root->mnt, root->node, &st) == 0 && st.ino == ino) ||
               (ino == 1 && mnt_is_root(root->mnt) && root->node == krffs_fs->node)) {
        /* The KRFFS root directory's stat reports inode 1 (`do_stat64`). */
        rc = 0;
    } else if (vfs_enter(root->mnt) == 0) {
        dir = *root;
        vfs_ref_hold(&dir);
        depth = handle_push(&lv[0], &dir, 0) == 0 ? 0 : -1;
        while (depth >= 0 && rc != 0) {
            struct handle_level *l = &lv[depth];

            if (vfs_readdir(l->dir.mnt, l->h, &l->cursor, de) <= 0) {
                handle_pop(&lv[depth--]);
            } else if (handle_entry(lv, &depth, de, root, ino, out)) {
                rc = 0;
            }
        }
        while (depth >= 0) {
            handle_pop(&lv[depth--]);
        }
    }
    kfree(lv);
    kfree(de);
    return rc;
}

/**
 * Read the user's handle `uh` for open_by_handle_at, of a file of the mount
 * `m`, into `*ino`: 0, or -EFAULT, -EINVAL for a handle_bytes of 0 or above
 * MAX_HANDLE_SZ or a negative handle_type, or -ESTALE for a handle of
 * another kind or size or a mount whose files do not reopen.
 */
static int32_t handle_read(const struct linux_file_handle __user *uh, const struct vfs_mount *m,
                           uint64_t *ino)
{
    struct linux_file_handle h;
    unsigned char bytes[HANDLE_BYTES];
    uint32_t i;

    if (copy_from_user(&h, uh, sizeof(h)) != 0) {
        return -EFAULT;
    }
    if (h.handle_bytes == 0 || h.handle_bytes > MAX_HANDLE_SZ || h.handle_type < 0) {
        return -EINVAL;
    }
    if (h.handle_type != FILEID_INO64_GEN || h.handle_bytes != HANDLE_BYTES ||
        !handle_decodable(m)) {
        return -ESTALE;
    }
    if (copy_from_user(bytes, (const char __user *)uh + sizeof(h), sizeof(bytes)) != 0) {
        return -EFAULT;
    }
    *ino = 0;
    for (i = 0; i < 8; i++) {
        *ino |= (uint64_t)bytes[i] << (8 * i);
    }
    return 0;
}

/**
 * The path from the caller's root of the file of inode `ino` on the mount
 * `m` into `path`, a path page: the mount's root's path and the path
 * `handle_find` gives below it. 0, or -ESTALE for a mount that is going, a
 * file it does not find or one outside the caller's root, -ENAMETOOLONG,
 * or -ENOMEM.
 */
static int32_t handle_path(struct vfs_mount *m, uint64_t ino, char *path)
{
    char *rel = path_buf_get();
    struct vfs_ref root;
    uint32_t n;
    int32_t rc;

    if (!rel) {
        return -ENOMEM;
    }
    root.mnt = m;
    root.node = m->root;
    rc = vfs_enter(m) != 0 ? -ESTALE : 0;
    if (rc == 0) {
        vfs_ref_hold(&root);
        rc = vfs_node_path(m, m->root, path, VFS_PATH_MAX);
        if (rc == 0) {
            rc = handle_find(&root, ino, rel);
        }
        vfs_ref_release(&root);
    }
    if (rc == 0 && path_in_root(path) != 0) {
        rc = -ESTALE;
    }
    n = rc == 0 ? (uint32_t)k_strlen(path) : 0;
    if (rc == 0 && rel[0] && n + 1 + k_strlen(rel) + 1 > VFS_PATH_MAX) {
        rc = -ENAMETOOLONG;
    }
    if (rc == 0 && rel[0]) {
        if (n > 1) {
            path[n++] = '/';
        }
        k_memcpy(path + n, rel, k_strlen(rel) + 1);
    }
    path_buf_put(rel);
    return rc;
}

/**
 * open_by_handle_at takes the descriptor of any file of the mount, the
 * struct file_handle name_to_handle_at gave, and open flags.
 *
 * The mount is that of what the descriptor refers to, the working
 * directory's for AT_FDCWD. The file with the handle's inode is found on it
 * (`handle_find`) and opened by its path from the caller's root as open(2)
 * opens it, with `flags` less O_CREAT and O_EXCL, and O_NOFOLLOW, so that a
 * symbolic link opens only with O_PATH, as Linux opens the inode itself. The
 * caller must be privileged, as Linux asks CAP_DAC_READ_SEARCH.
 *
 * The descriptor, or -EBADF for a descriptor that is not open, -EPERM for an
 * unprivileged caller, -EFAULT, -EINVAL for a handle_bytes of 0 or above
 * MAX_HANDLE_SZ or a negative handle_type, -ESTALE for a handle of another
 * kind or size, of a mount whose files do not reopen, or of a file that is
 * gone or outside the caller's root, -ENOMEM, or open(2)'s error.
 */
long sys_open_by_handle_at(isr_params_t *params)
{
    const struct linux_file_handle __user *uh =
        (const struct linux_file_handle __user *)SC_ARG(params, 1);
    int flags = (int)SC_ARG(params, 2) & ~(O_CREAT | O_EXCL);
    struct kstat kst;
    struct vfs_mount *m;
    uint64_t ino = 0;
    char *path;
    int32_t rc;

    if (!current_proc) {
        return -EFAULT;
    }
    rc = stat_at((int)SC_ARG(params, 0), NULL, AT_EMPTY_PATH, &kst);
    if (rc != 0) {
        return rc;
    }
    if (!proc_fs_privileged(current_proc)) {
        return -EPERM;
    }
    m = kst.st_mnt_id ? vfs_mount_at((int)kst.st_mnt_id - 1) : NULL;
    rc = handle_read(uh, m, &ino);
    if (rc != 0) {
        return rc;
    }
    path = path_buf_get();
    if (!path) {
        return -ENOMEM;
    }
    rc = handle_path(m, ino, path);
    if (rc == 0) {
        rc = open_named(path_name(path), flags | O_NOFOLLOW, 0, 0);
        if (rc >= 0) {
            open_done(rc, path_name(path), flags);
        }
    }
    path_buf_put(path);
    return rc;
}

/* access(2) mode bits. */
#define F_OK 0

#define X_OK 1

#define W_OK 2

#define R_OK 4

/**
 * The core of access(2), faccessat(2) and faccessat2(2): 0 when the file at
 * `name`, leading '/' stripped, exists and grants `mode`, an OR of R_OK,
 * W_OK and X_OK, or F_OK alone, to the caller's real ids, or to its
 * effective ids with AT_EACCESS in `flags`; otherwise a negative errno:
 * -EINVAL for any other bit in `mode`, the walk's error, -EACCES, or -EROFS
 * for W_OK on a read-only mounted volume. A final symbolic link is
 * followed unless `flags` has AT_SYMLINK_NOFOLLOW, and a path that named a
 * directory by its end, `dir`, must reach one (`resolve_named`).
 *
 * access(2) is the one call judged by the real ids: it exists so that a set-id
 * program can ask what its invoker could do. The walk charges search permission
 * the same way, so a set-id program cannot report a reachability its invoker
 * does not have. A /dev, /proc or /sys node is judged by the owner, group and
 * mode its stat gives, through a final link, so W_OK on /proc/PID/stat is
 * -EACCES to a caller other than root, and R_OK succeeds. R_OK, W_OK and X_OK
 * are the octal digits the permission check takes. The walk keeps the lock of
 * the mount it ends on until the attributes are read, as `do_stat64`'s does,
 * and for the same reason.
 */
static int32_t do_access(const char *name, int mode, int dir, int flags)
{
    int real = !(flags & AT_EACCESS);
    int follow = !(flags & AT_SYMLINK_NOFOLLOW);
    struct vfs_ref ref;
    const struct vpath *vp;
    int32_t rc;
    int werr;

    if (mode & ~(R_OK | W_OK | X_OK)) {
        return -EINVAL;
    }
    werr = resolve_named(
        name, (real ? VFS_WALK_REAL_IDS : 0) | VFS_WALK_LOCK | (follow ? 0 : VFS_WALK_NOFOLLOW),
        dir, &ref);
    if (werr != 0) {
        return werr;
    }
    vp = vfs_synth_vpath(ref.mnt, ref.node);
    if (vp) {
        struct kstat s;
        int ro = (mode & W_OK) && proc_read_only(&ref, 1, 1);

        rc = synth_stat(vp, &s, follow || dir);
        vfs_walk_unlock(ref.mnt);
        vfs_ref_release(&ref);
        if (rc != 0) {
            return rc;
        }
        rc =
            cred_access_bits((uint32_t)s.st_uid, (uint32_t)s.st_gid, s.st_mode & 0777u, mode, real);
        return rc == 0 && ro ? -EROFS : rc;
    }
    if (!mnt_is_root(ref.mnt)) {
        rc = cred_access_ref(&ref, mode, real);
        if (rc == 0 && (mode & W_OK) && (ref.mnt->flags & VFS_MS_RDONLY)) {
            rc = -EROFS;
        }
    } else {
        rc = cred_access_node((krffs_node_t *)ref.node, mode, real);
    }
    vfs_walk_unlock(ref.mnt);
    vfs_ref_release(&ref);
    return rc;
}

/**
 * `do_access` for a caller without the big lock: a path the name cache
 * finishes (`vfs_resolve_cached`), absolute or relative to the working
 * directory (`path_from_user_unlocked`), to a file of a type that sets
 * `reads_unlocked`, whose attributes and permission are read under the
 * file's own lock, as Linux's faccessat walks a path under RCU first
 * (fs/open.c:465-535, fs/namei.c:2651-2653). The walk charges search
 * permission to the real ids unless `flags` has AT_EACCESS, as
 * `do_access`'s does. 0, a negative errno, or CALL_LOCKED with nothing
 * done, for `do_access` under the big lock: a `mode` with another bit, whose
 * -EINVAL that call gives first, among them.
 */
static int32_t access_unlocked(int dirfd, const char __user *upath, int mode, int flags)
{
    int real = !(flags & AT_EACCESS);
    int follow = !(flags & AT_SYMLINK_NOFOLLOW);
    struct vfs_ref ref;
    struct vfs_stat st;
    char *path;
    int dir = 0;
    int32_t rc;

    if (mode & ~(R_OK | W_OK | X_OK)) {
        return CALL_LOCKED;
    }
    rc = path_from_user_unlocked(dirfd, upath, &path, &dir);
    if (rc != 0) {
        return rc;
    }
    rc = vfs_resolve_cached(path,
                            (real ? VFS_WALK_REAL_IDS : 0) | VFS_WALK_LOCK |
                                (follow || dir ? 0 : VFS_WALK_NOFOLLOW),
                            &ref, NULL);
    path_buf_put(path);
    if (rc != 0) {
        return rc == VFS_CACHED_MISS ? CALL_LOCKED : rc;
    }
    if (!vfs_mount_base(ref.mnt)->type->reads_unlocked) {
        rc = CALL_LOCKED;
    } else if (dir) {
        rc = vfs_getattr(ref.mnt, ref.node, &st);
        rc = rc != 0 ? rc : ((st.mode & S_IFMT) == S_IFDIR ? 0 : -ENOTDIR);
    }
    if (rc == 0) {
        rc = cred_access_ref(&ref, mode, real);
    }
    if (rc == 0 && (mode & W_OK) && (ref.mnt->flags & VFS_MS_RDONLY)) {
        rc = -EROFS;
    }
    vfs_walk_unlock(ref.mnt);
    vfs_ref_release(&ref);
    return rc;
}

/**
 * The access check of the user's path `upath`, relative to `dirfd`, for
 * `mode` and `flags` (`do_access`): without the big lock where
 * `access_unlocked` finishes it, and under it otherwise. 0 or a negative
 * errno.
 */
static int32_t access_path(int dirfd, const char __user *upath, int mode, int flags)
{
    char *path;
    int dir, big;
    int32_t rc;

    big = !bkl_held();
    if (big) {
        rc = access_unlocked(dirfd, upath, mode, flags);
        if (rc != CALL_LOCKED) {
            return rc;
        }
        bkl_enter();
    }
    rc = path_from_user_dir(dirfd, upath, &path, &dir);
    if (rc == 0) {
        rc = do_access(path_name(path), mode, dir, flags);
        path_buf_put(path);
    }
    if (big) {
        bkl_exit();
    }
    return rc;
}

/**
 * access(2): whether the caller's real ids may use the file at the user's
 * path in argument 0 as the mode in argument 1 asks (`access_path`); 0 or a
 * negative errno, -EFAULT for a NULL path. The call runs without the big
 * lock, which `access_path` takes for what the name cache leaves.
 */
long sys_access(isr_params_t *params)
{
    const char __user *upath = (const char __user *)SC_ARG(params, 0);

    if (!current_proc || !upath) {
        return -EFAULT;
    }
    return access_path(AT_FDCWD, upath, (int)SC_ARG(params, 1), 0);
}

/**
 * inotify_add_watch(2): add a watch of the file at the user's path in
 * argument 1, for the events of the mask in argument 2, to the inotify
 * instance at the descriptor in argument 0; the watch descriptor, or a
 * negative errno.
 *
 * The mask is checked first: IN_MASK_ADD with IN_MASK_CREATE, or a mask
 * with no event, is -EINVAL. Then the descriptor: -EBADF when it is not
 * open, -EINVAL when it is no inotify instance. The path is resolved
 * through a final link unless IN_DONT_FOLLOW, it must name a directory
 * under IN_ONLYDIR (-ENOTDIR), and the caller must be able to read what it
 * names (-EACCES), as on Linux; `inotify_watch` adds the watch, with its
 * own errors. A /proc/PID/fd/N link that is followed watches the file or
 * directory its descriptor is open on (`proc_fd_file`), as Linux does, so
 * that `udevadm settle`'s watch of /run/udev through an O_PATH descriptor
 * hears the queue's removal. A node of /dev, /proc or /sys can be watched
 * and reports nothing, since no call that changes one tells inotify.
 */
long sys_inotify_add_watch(isr_params_t *params)
{
    int fd = (int)SC_ARG(params, 0);
    const char __user *upath = (const char __user *)SC_ARG(params, 1);
    uint32_t mask = (uint32_t)SC_ARG(params, 2);
    const struct vpath *vp;
    struct vfs_ref ref;
    uint64_t ino = 0;
    int isdir = 0;
    char *path;
    int dir;
    int32_t rc;

    if (((mask & IN_MASK_ADD) && (mask & IN_MASK_CREATE)) || !(mask & IN_ALL_EVENTS)) {
        return -EINVAL;
    }
    if (!current_proc || !fd_get(current_proc->fds, fd)) {
        return -EBADF;
    }
    if (!inotify_is(fd)) {
        return -EINVAL;
    }
    if (!upath) {
        return -EFAULT;
    }
    rc = path_from_user_dir(AT_FDCWD, upath, &path, &dir);
    if (rc != 0) {
        return rc;
    }
    rc = resolve_named(path_name(path), (mask & IN_DONT_FOLLOW) ? VFS_WALK_NOFOLLOW : 0, dir, &ref);
    path_buf_put(path);
    if (rc != 0) {
        return rc;
    }
    vp = vfs_synth_vpath(ref.mnt, ref.node);
    if (vp && vp->kind == VNODE_PROC_FD_LINK && !(mask & IN_DONT_FOLLOW)) {
        struct vfs_ref obj;
        struct open_file *held;

        if (proc_fd_file(vp, &obj, &held) == 0) {
            vfs_walk_unlock(obj.mnt);
            fd_ofd_unref(held);
            vfs_ref_release(&ref);
            ref = obj;
            vp = NULL;
        }
    }
    if (vp) {
        struct kstat s;

        rc = synth_stat(vp, &s, !(mask & IN_DONT_FOLLOW));
        if (rc == 0) {
            rc = cred_access_bits((uint32_t)s.st_uid, (uint32_t)s.st_gid, s.st_mode & 0777u, CRED_R,
                                  0);
            ino = s.st_ino;
            isdir = (s.st_mode & S_IFMT) == S_IFDIR;
        }
    } else {
        struct vfs_stat st;

        rc = mnt_is_root(ref.mnt) ? (int32_t)cred_access_node((krffs_node_t *)ref.node, CRED_R, 0)
                                  : (int32_t)cred_access_ref(&ref, CRED_R, 0);
        if (rc == 0) {
            rc = vfs_getattr(ref.mnt, ref.node, &st);
            ino = st.ino;
            isdir = (st.mode & S_IFMT) == S_IFDIR;
        }
    }
    if (rc == 0 && (mask & IN_ONLYDIR) && !isdir) {
        rc = -ENOTDIR;
    }
    if (rc == 0) {
        rc = inotify_watch(fd, ref.mnt, ino, isdir, mask);
    }
    vfs_ref_release(&ref);
    return rc;
}

/**
 * Whether the caller may use what the descriptor `entry` is open on as
 * `mode` asks, for faccessat2's AT_EMPTY_PATH: the nine-bit rule over the
 * owner, group and mode fstat reports, by the real ids unless `flags` has
 * AT_EACCESS, X_OK on anything but a directory needing an execute bit even
 * for root, and W_OK on a read-only mounted volume -EROFS. Returns 0 or a
 * negative errno.
 */
static int32_t access_fd(fd_entry_t *entry, int mode, int flags)
{
    struct kstat st;
    int32_t rc;

    if (mode & ~(R_OK | W_OK | X_OK)) {
        return -EINVAL;
    }
    rc = fstat_fill(entry, &st);
    if (rc != 0) {
        return rc;
    }
    rc = cred_access_bits((uint32_t)st.st_uid, (uint32_t)st.st_gid, st.st_mode & 0777u, mode,
                          !(flags & AT_EACCESS));
    if (rc == 0 && (mode & X_OK) && (st.st_mode & S_IFMT) != S_IFDIR && !(st.st_mode & 0111u)) {
        rc = -EACCES;
    }
    if (rc == 0 && (mode & W_OK) && entry->ofd && entry->ofd->vmnt &&
        (entry->ofd->vmnt->flags & VFS_MS_RDONLY)) {
        rc = -EROFS;
    }
    return rc;
}

/**
 * faccessat2's AT_EMPTY_PATH: what `dirfd` is open on (`access_fd`), or the
 * working directory for AT_FDCWD (`do_access`). The caller holds the big
 * lock. 0 or a negative errno.
 */
static int32_t access_empty(int dirfd, int mode, int flags)
{
    char *path;
    int32_t rc;

    if (dirfd != AT_FDCWD) {
        fd_entry_t *entry = fd_get(current_proc->fds, dirfd);

        return entry ? access_fd(entry, mode, flags) : -EBADF;
    }
    path = path_buf_get();
    if (!path) {
        return -ENOMEM;
    }
    k_strncpy(path, proc_cwd(current_proc), VFS_PATH_MAX);
    rc = do_access(path_name(path), mode, 0, flags);
    path_buf_put(path);
    return rc;
}

/**
 * The body of faccessat(2) and faccessat2(2): access(2) of the user's path
 * `upath` relative to the directory descriptor `dirfd`, or to the working
 * directory for AT_FDCWD, by `do_access` with `flags`. With AT_EMPTY_PATH an
 * empty or null path names what `dirfd` is open on (`access_fd`), or the
 * working directory for AT_FDCWD. Any other flag is -EINVAL, a null path
 * without AT_EMPTY_PATH -EFAULT. A path runs without the big lock where the
 * name cache finishes it (`access_path`); an empty one takes it.
 */
static int32_t access_at(int dirfd, const char __user *upath, int mode, int flags)
{
    int empty = 0, big;
    int32_t rc;

    if (!current_proc) {
        return -ESRCH;
    }
    if (flags & ~(AT_EACCESS | AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH)) {
        return -EINVAL;
    }
    if (flags & AT_EMPTY_PATH) {
        empty = user_path_empty(upath);
        if (empty < 0) {
            return empty;
        }
    }
    if (!empty) {
        return upath ? access_path(dirfd, upath, mode, flags) : -EFAULT;
    }
    big = !bkl_held();
    if (big) {
        bkl_enter();
    }
    rc = access_empty(dirfd, mode, flags);
    if (big) {
        bkl_exit();
    }
    return rc;
}

/**
 * faccessat(2): access(2) of a path relative to the directory descriptor in
 * argument 0, or to the working directory for AT_FDCWD; the path and the
 * mode are arguments 1 and 2. Linux's call has no flags; libc's flags go
 * to faccessat2.
 */
long sys_faccessat(isr_params_t *params)
{
    return access_at((int)SC_ARG(params, 0), (const char __user *)SC_ARG(params, 1),
                     (int)SC_ARG(params, 2), 0);
}

/**
 * faccessat2(2): faccessat with the flags in argument 3, AT_EACCESS to
 * judge by the effective ids, AT_SYMLINK_NOFOLLOW to judge a final symbolic
 * link itself, and AT_EMPTY_PATH (`access_at`).
 */
long sys_faccessat2(isr_params_t *params)
{
    return access_at((int)SC_ARG(params, 0), (const char __user *)SC_ARG(params, 1),
                     (int)SC_ARG(params, 2), (int)SC_ARG(params, 3));
}

/**
 * chmod of the node `node` of the KRFFS boot root: the owner or a
 * privileged caller (`cred_privileged`) replaces its permission bits with
 * those of `mode`, which stamps its attribute-change time, and anyone else
 * is -EPERM; 0 on success. The root node's mode is never changed,
 * since its set-user-ID bit is the volume's dirty flag (KRFFS_MODE_DIRTY),
 * but the rule still answers, so a caller who does not own `/` gets -EPERM.
 *
 * An unprivileged caller who stores a mode on a file whose group is not one
 * of the caller's loses setgid, as chmod(2) has it; a directory keeps it.
 * That is the third set-id clear POSIX names, after write and chown, and
 * the one no clear at a write can stand for, since an owner could make a
 * gid-0 file 02710 by chmod alone and never write it. The clear does not
 * depend on group-execute, as `cred_kill_setid`'s does, since the caller
 * chooses that bit in the same call. Setuid needs no rule here: only the
 * owner may chmod, and the bit grants that owner's own identity.
 */
static int32_t krffs_chmod_node(krffs_node_t *node, uint32_t mode)
{
    if (!cred_fs_privileged() && proc_fsuid(current_proc) != node->uid) {
        return -EPERM;
    }
    if (node != krffs_fs->node) {
        uint32_t want = mode & 07777u;

        if (!cred_fs_privileged() && !KRFFS_IS_DIR(node->mode) && !cred_in_group(node->gid)) {
            want &= ~(uint32_t)KRFFS_MODE_SGID;
        }
        node->mode = (uint16_t)((node->mode & ~07777u) | want);
        krffs_stamp(node, KRFFS_STAMP_ATTR);
    }
    return 0;
}

/**
 * chown of the node `node` of the KRFFS boot root, the POSIX split: a
 * change of owner takes a privileged caller, and a change of group takes a
 * privileged caller or the owner naming one of the owner's own groups;
 * otherwise -EPERM, and 0 on success. `(id_t)-1` keeps a field, and a value
 * that changes nothing needs no right, so `chown(path, -1, -1)` succeeds on
 * any file that exists.
 *
 * A call that changes either field stamps the node's attribute-change time
 * once, after both, and clears its set-id bits (`cred_kill_setid`) whoever
 * asks, as POSIX has it, which is why cp and install chown before they
 * chmod. Both happen only when a field changes: `chown(path, -1, -1)`
 * succeeds for every caller and leaves the times alone, and a clear that
 * ran on it would let any user clear the set-user-ID bit of /bin/su with
 * one call.
 */
static int32_t krffs_chown_node(krffs_node_t *node, uint32_t uid, uint32_t gid)
{
    int uid_changes = (uid != (uint32_t)-1 && uid != node->uid);
    int gid_changes = (gid != (uint32_t)-1 && gid != node->gid);

    if (uid_changes && !cred_fs_privileged()) {
        return -EPERM;
    }
    if (gid_changes && !cred_fs_privileged() &&
        (proc_fsuid(current_proc) != node->uid || !cred_in_group(gid))) {
        return -EPERM;
    }
    if (uid != (uint32_t)-1) {
        node->uid = uid;
    }
    if (gid != (uint32_t)-1) {
        node->gid = gid;
    }
    if (uid_changes || gid_changes) {
        krffs_stamp(node, KRFFS_STAMP_ATTR);
        cred_kill_setid(node);
    }
    return 0;
}

/**
 * The core of chmod(2) and fchmodat(2): set the permission bits of the file
 * at `name`, leading '/' stripped, through a final symbolic link, to those
 * of `mode`; 0 or a negative errno. A path that named a directory by its
 * end, `dir`, must reach one, as in `do_access`.
 *
 * A node of the KRFFS boot root follows `krffs_chmod_node`. A node of a
 * mounted type follows the same rule, the owner or a privileged caller and
 * otherwise -EPERM, and an unprivileged caller loses setgid on a file or a
 * directory whose group is not one of the caller's; the read, the check
 * and the store are one hold of the mount's namespace lock. A pty slave
 * (/dev/pts/N) stores the bits on the pty, its owner or a privileged caller
 * only, which dropbear does before it hands the terminal to the user;
 * -ENOENT when the pty is gone. An entry root made under /dev keeps its
 * own (`chmod_made`). Every other /dev, /proc or /sys node has fixed
 * metadata: a caller whose file-system uid is 0 succeeds without a change,
 * and anyone else is -EPERM.
 *
 * The walk takes the lock of the mount it ends on, where the type asks for
 * one, and the lookup, the check and the store are one hold of it: a node of
 * a mounted KRFFS volume is its offset, which a growing write that held the
 * lock would move, and a store made at the offset the walk found would
 * change whatever the volume put there since.
 */
/**
 * chmod of the entry `id` root made under /dev (devtree.h): its owner or a
 * privileged caller sets its permission bits, an unprivileged one losing
 * setgid on an entry whose group is not one of the caller's. 0, -ENOENT for
 * an entry that is gone, or -EPERM.
 */
static int32_t chmod_made(int id, uint32_t mode)
{
    struct devtree_attr a;

    if (devtree_attr(id, &a) != 0) {
        return -ENOENT;
    }
    if (!cred_fs_privileged() && proc_fsuid(current_proc) != a.uid) {
        return -EPERM;
    }
    if (!cred_fs_privileged() && !cred_in_group(a.gid)) {
        mode &= ~02000u;
    }
    return devtree_setattr(id, mode & 07777u, (uint32_t)-1, (uint32_t)-1);
}

/**
 * chown of the entry `id` root made under /dev: a new owner takes a
 * privileged caller, and a new group a privileged caller or the owner
 * naming one of its groups, as on Linux; -1 leaves a field. 0, -ENOENT for
 * an entry that is gone, or -EPERM.
 */
static int32_t chown_made(int id, uint32_t uid, uint32_t gid)
{
    struct devtree_attr a;

    if (devtree_attr(id, &a) != 0) {
        return -ENOENT;
    }
    if (!cred_fs_privileged() && ((uid != (uint32_t)-1 && uid != a.uid) ||
                                  (gid != (uint32_t)-1 && gid != a.gid &&
                                   (proc_fsuid(current_proc) != a.uid || !cred_in_group(gid))))) {
        return -EPERM;
    }
    return devtree_setattr(id, (uint32_t)-1, uid, gid);
}

/**
 * chmod of the device `name` under /dev: its owner or a privileged caller
 * sets its permission bits, as udev sets those of Linux's devtmpfs nodes
 * (`devfs_setattr`); 0, -ENOENT, -ENOSPC or -EPERM.
 */
static int32_t chmod_dev(const char *name, uint32_t mode)
{
    uint32_t uid, gid;

    if (devfs_owner(name, &uid, &gid) != 0) {
        return -ENOENT;
    }
    if (!cred_fs_privileged() && proc_fsuid(current_proc) != uid) {
        return -EPERM;
    }
    return devfs_setattr(name, mode & 0777u, (uint32_t)-1, (uint32_t)-1);
}

/**
 * chown of the device `name` under /dev, by the rule of `chown_made`: a new
 * owner takes a privileged caller, and a new group a privileged caller or
 * the owner naming one of its groups; -1 leaves a field. 0, -ENOENT, -ENOSPC
 * or -EPERM.
 */
static int32_t chown_dev(const char *name, uint32_t uid, uint32_t gid)
{
    uint32_t ouid, ogid;

    if (devfs_owner(name, &ouid, &ogid) != 0) {
        return -ENOENT;
    }
    if (!cred_fs_privileged() && ((uid != (uint32_t)-1 && uid != ouid) ||
                                  (gid != (uint32_t)-1 && gid != ogid &&
                                   (proc_fsuid(current_proc) != ouid || !cred_in_group(gid))))) {
        return -EPERM;
    }
    return devfs_setattr(name, (uint32_t)-1, uid, gid);
}

static int32_t chmod_fd(fd_entry_t *entry, uint32_t mode);

/**
 * chmod of the /dev, /proc or /sys node `vp`: a pty slave on the pty, its
 * owner or a privileged caller only, -ENOENT when the pty is gone; an entry
 * root made under /dev by `chmod_made`; a device by `chmod_dev`; and a
 * /proc/PID/fd/N link on the descriptor it names (`chmod_fd`), as Linux
 * follows the link, which udev's chmod of an O_PATH descriptor takes. Every
 * other node has fixed metadata, which root's request leaves as it is.
 */
static int32_t chmod_synth(const struct vpath *vp, uint32_t mode)
{
    if (vp->kind == VNODE_PTS) {
        struct pty *pt = pty_get(vp->pts_index);
        if (!pt) {
            return -ENOENT;
        }
        if (!cred_fs_privileged() && proc_fsuid(current_proc) != pt->uid) {
            return -EPERM;
        }
        pt->mode = (uint16_t)(mode & 0777u);
        return 0;
    }
    if (vp->kind == VNODE_DEV_MADE) {
        return chmod_made(vp->syskey, mode);
    }
    if (vp->kind == VNODE_DEV && vp->name[0] != '\0') {
        return chmod_dev(vp->name, mode);
    }
    if (vp->kind == VNODE_PROC_FD_LINK) {
        process_t *p;
        fd_entry_t src;
        int32_t rc = proc_fd_entry(vp, &p, &src);

        if (rc == 0) {
            rc = chmod_fd(&src, mode);
            fd_peek_drop(&src);
        }
        return rc;
    }
    return proc_fsuid(current_proc) == 0 ? 0 : -EPERM; /* fixed metadata root owns */
}

/**
 * Set the mode of the file `name`, resolved against `dir`, for chmod,
 * fchmodat and fchmodat2.  A nonzero `follow` follows a last symbolic link;
 * zero gives -EOPNOTSUPP for a link, as Linux does, since a link has no mode
 * of its own.  Returns 0 or a negative errno.
 */
static int32_t do_chmod(const char *name, uint32_t mode, int dir, int follow)
{
    struct vpath vp;
    const struct vpath *svp;
    struct vfs_ref ref;
    krffs_node_t *node;
    int32_t rc;

    rc = resolve_named(name, VFS_WALK_LOCK | (follow ? 0 : VFS_WALK_NOFOLLOW), dir, &ref);
    if (rc != 0) {
        return rc;
    }
    svp = vfs_synth_vpath(ref.mnt, ref.node);
    if (svp) {
        vp = *svp;
        vfs_walk_unlock(ref.mnt);
        vfs_ref_release(&ref);
        if (!follow && synth_is_link(&vp)) {
            return -EOPNOTSUPP; /* a link's mode does not change, as on Linux */
        }
        return chmod_synth(&vp, mode);
    }
    if (!mnt_is_root(ref.mnt)) {
        struct vfs_stat vst;

        vfs_node_lock(&ref);
        rc = vfs_getattr(ref.mnt, ref.node, &vst);
        if (rc == 0 && !follow && (vst.mode & 0170000u) == 0120000u) {
            rc = -EOPNOTSUPP;
        }
        if (rc == 0 && !cred_fs_privileged() && proc_fsuid(current_proc) != vst.uid) {
            rc = -EPERM;
        }
        if (rc == 0) {
            if (!cred_fs_privileged() && !cred_in_group(vst.gid)) {
                mode &= ~02000u;
            }
            vst.mode = mode;
            rc = vfs_setattr(ref.mnt, ref.node, VFS_SET_MODE, &vst);
        }
        vfs_node_unlock(&ref);
        vfs_walk_unlock(ref.mnt);
        if (rc == 0) {
            attr_told(name, ref.mnt, ref.node, IN_ATTRIB, 0);
        }
        vfs_ref_release(&ref);
        return rc;
    }
    node = ref.node;
    rc = krffs_chmod_node(node, mode);
    vfs_walk_unlock(ref.mnt);
    if (rc == 0) {
        attr_told(name, ref.mnt, ref.node, IN_ATTRIB, 1);
    }
    vfs_ref_release(&ref);
    return rc;
}

/**
 * Whether a chown of `uid` and `gid` names an owner or a group, which makes
 * it a change of the attributes for inotify even when the file has them
 * already, as Linux's `fsnotify_change` reports it; -1 for both is none.
 */
static int chown_asks(uint32_t uid, uint32_t gid)
{
    return uid != (uint32_t)-1 || gid != (uint32_t)-1;
}

/**
 * What a chown changes on a mounted file system, and the set-id bits it
 * takes with it.
 *
 * `vst` is read from the node and updated in place; the return is the
 * `VFS_SET_*` mask to apply, 0 when the call changes nothing, or `-EPERM`
 * when this caller may not make the change. A chown that changes neither
 * owner is permitted to every caller, as it is on the boot root: a value
 * that changes nothing needs no right, and a refusal there would stop
 * `chown(path, -1, -1)`. The set-id clear is `vfs_kill_setid`'s rule, which
 * is `cred_kill_setid`'s rule for the boot root: a directory keeps its bits,
 * and a file without group-execute keeps setgid, since there the bit marks
 * mandatory locking and grants nobody anything.
 */
static int vfs_chown_plan(struct vfs_stat *vst, uint32_t uid, uint32_t gid)
{
    int what = 0;

    if (uid != (uint32_t)-1 && uid != vst->uid) {
        what |= VFS_SET_UID;
    }
    if (gid != (uint32_t)-1 && gid != vst->gid) {
        what |= VFS_SET_GID;
    }
    if (what == 0) {
        return 0;
    }
    if (!cred_fs_privileged() &&
        (proc_fsuid(current_proc) != vst->uid || (what & VFS_SET_UID) != 0 ||
         ((what & VFS_SET_GID) != 0 && !cred_in_group(gid)))) {
        return -EPERM;
    }
    if (what & VFS_SET_UID) {
        vst->uid = uid;
    }
    if (what & VFS_SET_GID) {
        vst->gid = gid;
    }
    if ((vst->mode & 06000u) && (vst->mode & 0170000u) != 0040000u) {
        vst->mode &= ~04000u;
        if (vst->mode & 0010u) {
            vst->mode &= ~02000u;
        }
        what |= VFS_SET_MODE;
    }
    return what;
}

static int32_t chown_fd(fd_entry_t *entry, uint32_t uid, uint32_t gid);

/**
 * chown of the /dev or /proc node `vp`, for the path forms and for a
 * descriptor opened with O_PATH on one. A pty slave keeps its owner and group
 * on the pty, and changing either takes root, since the login server that
 * chowns a pty is root. An entry root made under /dev keeps its own
 * (`chown_made`). Every other node has fixed root ownership: a request
 * for 0 or -1 in each field succeeds and changes nothing, and any other is
 * -EPERM.
 */
static int32_t chown_synth(const struct vpath *vp, uint32_t uid, uint32_t gid)
{
    if (vp->kind == VNODE_PROC_FD_LINK) {
        process_t *p;
        fd_entry_t src;
        int32_t rc = proc_fd_entry(vp, &p, &src);

        if (rc == 0) {
            rc = chown_fd(&src, uid, gid);
            fd_peek_drop(&src);
        }
        return rc;
    }
    if (vp->kind == VNODE_DEV_MADE) {
        return chown_made(vp->syskey, uid, gid);
    }
    if (vp->kind == VNODE_DEV && vp->name[0] != '\0') {
        return chown_dev(vp->name, uid, gid);
    }
    if (vp->kind == VNODE_PTS) {
        struct pty *pt = pty_get(vp->pts_index);
        int uid_changes;
        int gid_changes;

        if (!pt) {
            return -ENOENT;
        }
        uid_changes = (uid != (uint32_t)-1 && uid != pt->uid);
        gid_changes = (gid != (uint32_t)-1 && gid != pt->gid);
        if ((uid_changes || gid_changes) && !cred_fs_privileged()) {
            return -EPERM;
        }
        if (uid != (uint32_t)-1) {
            pt->uid = uid;
        }
        if (gid != (uint32_t)-1) {
            pt->gid = gid;
        }
        return 0;
    }
    if ((uid != (uint32_t)-1 && uid != 0) || (gid != (uint32_t)-1 && gid != 0)) {
        return -EPERM;
    }
    return 0;
}

/**
 * chown core for the path forms: the file at `name`, leading '/' stripped,
 * through a final symbolic link when `follow`, as chown(2) and fchownat(2)
 * without AT_SYMLINK_NOFOLLOW go, and the link itself otherwise, as lchown(2)
 * and fchownat(2) with the flag go. A /dev or /proc node is `chown_synth`'s,
 * a node of a mounted type follows `vfs_chown_plan`, and a KRFFS node
 * `krffs_chown_node`; the boot root has no links, so there `follow` changes
 * nothing. A path that named a directory by its end, `dir`, follows a final
 * link either way and must reach a directory (`resolve_named`). The walk
 * keeps its mount's lock through the store, as `do_chmod`'s does, and for
 * the same reason.
 *
 * On a mounted type the read, the plan and the store are one step under
 * the mount's namespace lock. Were the lock taken inside `vfs_setattr`, the
 * plan would be made from attributes another process can change while this
 * one waits for the lock: a privileged chown planned over a 0755 file would
 * store the new owner without the set-id clear the file earned from a chmod
 * that came in between, and leave a set-id program with an owner the caller
 * chose. The lock is recursive for its holder, so the calls under it take
 * it again for nothing.
 */
static int32_t do_chown(const char *name, uint32_t uid, uint32_t gid, int follow, int dir)
{
    struct vpath vp;
    const struct vpath *svp;
    struct vfs_ref ref;
    krffs_node_t *node;
    int32_t rc;

    rc = resolve_named(name, (follow ? 0 : VFS_WALK_NOFOLLOW) | VFS_WALK_LOCK, dir, &ref);
    if (rc != 0) {
        return rc;
    }
    svp = vfs_synth_vpath(ref.mnt, ref.node);
    if (svp) {
        vp = *svp;
        vfs_walk_unlock(ref.mnt);
        vfs_ref_release(&ref);
        return chown_synth(&vp, uid, gid);
    }
    if (!mnt_is_root(ref.mnt)) {
        struct vfs_stat vst;
        int what;

        vfs_node_lock(&ref);
        rc = vfs_getattr(ref.mnt, ref.node, &vst);
        what = 0;
        if (rc == 0) {
            what = vfs_chown_plan(&vst, uid, gid);
            rc = what < 0 ? what : (what ? vfs_setattr(ref.mnt, ref.node, what, &vst) : 0);
        }
        vfs_node_unlock(&ref);
        vfs_walk_unlock(ref.mnt);
        if (rc == 0 && chown_asks(uid, gid)) {
            attr_told(name, ref.mnt, ref.node, IN_ATTRIB, what == 0);
        }
        vfs_ref_release(&ref);
        return rc;
    }
    node = ref.node;
    rc = krffs_chown_node(node, uid, gid);
    vfs_walk_unlock(ref.mnt);
    if (rc == 0 && chown_asks(uid, gid)) {
        attr_told(name, ref.mnt, ref.node, IN_ATTRIB, 1);
    }
    vfs_ref_release(&ref);
    return rc;
}

/**
 * chmod(2): set the permission bits of the file at the user's path in
 * argument 0 to those of the mode in argument 1 (`do_chmod`); 0 or a
 * negative errno, -EFAULT for a NULL path.
 */
long sys_chmod(isr_params_t *params)
{
    const char __user *upath = (const char __user *)SC_ARG(params, 0);
    uint32_t mode = (uint32_t)SC_ARG(params, 1);
    char *path;
    int dir;
    int32_t rc;

    if (!current_proc || !upath) {
        return -EFAULT;
    }
    rc = path_from_user_dir(AT_FDCWD, upath, &path, &dir);
    if (rc != 0) {
        return rc;
    }
    rc = do_chmod(path_name(path), mode, dir, 1);
    path_buf_put(path);
    return rc;
}

/**
 * fchmodat(2): chmod(2) of a path relative to the directory descriptor in
 * argument 0, or to the working directory for AT_FDCWD; the path and the
 * mode are arguments 1 and 2. The system call takes no flags, and a final
 * symbolic link is followed.
 */
long sys_fchmodat(isr_params_t *params)
{
    int dirfd = (int)SC_ARG(params, 0);
    const char __user *upath = (const char __user *)SC_ARG(params, 1);
    uint32_t mode = (uint32_t)SC_ARG(params, 2);
    char *path;
    int dir;
    int32_t rc;

    if (!current_proc || !upath) {
        return -EFAULT;
    }
    rc = path_from_user_dir(dirfd, upath, &path, &dir);
    if (rc != 0) {
        return rc;
    }
    rc = do_chmod(path_name(path), mode, dir, 1);
    path_buf_put(path);
    return rc;
}

/**
 * fchmodat2(2): fchmodat(2) with Linux's flags in argument 3.
 * AT_SYMLINK_NOFOLLOW leaves a final symbolic link unfollowed, whose mode
 * does not change, -EOPNOTSUPP, and AT_EMPTY_PATH with an empty path
 * changes what `dirfd` is open on, an O_PATH descriptor's included, or the
 * working directory for AT_FDCWD; any other bit is -EINVAL. An empty path
 * without AT_EMPTY_PATH is -ENOENT, and a NULL one -EFAULT. systemd's
 * udevd sets a device node's mode through it.
 */
long sys_fchmodat2(isr_params_t *params)
{
    int dirfd = (int)SC_ARG(params, 0);
    const char __user *upath = (const char __user *)SC_ARG(params, 1);
    uint32_t mode = (uint32_t)SC_ARG(params, 2);
    int flags = (int)SC_ARG(params, 3);
    char *path;
    int empty = 0;
    int dir = 0;
    int32_t rc;

    if (!current_proc) {
        return -ESRCH;
    }
    if (flags & ~(AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH)) {
        return -EINVAL;
    }
    if (!upath) {
        return -EFAULT;
    }
    if (flags & AT_EMPTY_PATH) {
        empty = user_path_empty(upath);
        if (empty < 0) {
            return empty;
        }
    }
    if (empty && dirfd != AT_FDCWD) {
        fd_entry_t *entry = fd_get(current_proc->fds, dirfd);

        return entry ? chmod_fd(entry, mode) : -EBADF;
    }
    if (empty) {
        path = path_buf_get();
        if (!path) {
            return -ENOMEM;
        }
        k_strncpy(path, proc_cwd(current_proc), VFS_PATH_MAX);
    } else {
        rc = path_from_user_dir(dirfd, upath, &path, &dir);
        if (rc != 0) {
            return rc;
        }
    }
    rc = do_chmod(path_name(path), mode, dir, !(flags & AT_SYMLINK_NOFOLLOW));
    path_buf_put(path);
    return rc;
}

/**
 * fchmod(2): set the permission bits of what the descriptor in argument 0
 * is open on to those of the mode in argument 1, under the rules `do_chmod`
 * keeps; 0 or a negative errno. A node of the KRFFS boot root or of a
 * mounted type stores the mode, a pty slave stores it on the pty, -EBADF
 * when the pty is gone, and every other descriptor has fixed metadata, as
 * its path has: a caller whose file-system uid is 0 succeeds without a
 * change, and anyone else is -EPERM. A descriptor that is not open, or was
 * opened with O_PATH, is -EBADF, as on Linux.
 */
/**
 * fchmod of the descriptor `entry` when it is on no file of a file system:
 * a pty slave stores the bits on the pty under the owner-or-root rule
 * (-EBADF when the pty is gone), a directory made under /dev on the entry
 * (`chmod_made`), and any other has fixed metadata, which root's request
 * leaves as it is and anyone else's is -EPERM.
 */
static int32_t fchmod_synth(fd_entry_t *entry, uint32_t mode)
{
    if (entry->type == FD_TYPE_SYNTH_PATH) {
        return entry->priv ? chmod_synth((const struct vpath *)entry->priv, mode) : -EBADF;
    }
    if (synth_fd_made(entry)) {
        return chmod_made(synth_fd_made(entry), mode);
    }
    if (entry->type == FD_TYPE_DEVICE && devfs_fd_name(entry)) {
        return chmod_dev(devfs_fd_name(entry), mode);
    }
    if (entry->type == FD_TYPE_PTS) {
        struct pty *pt = pty_get(pty_slave_index(entry));
        if (!pt) {
            return -EBADF;
        }
        if (!cred_fs_privileged() && proc_fsuid(current_proc) != pt->uid) {
            return -EPERM;
        }
        pt->mode = (uint16_t)(mode & 0777u);
        return 0;
    }
    return proc_fsuid(current_proc) == 0 ? 0 : -EPERM;
}

long sys_fchmod(isr_params_t *params)
{
    int fd = (int)SC_ARG(params, 0);
    uint32_t mode = (uint32_t)SC_ARG(params, 1);
    fd_entry_t *entry;

    if (!current_proc) {
        return -ESRCH;
    }
    entry = fd_get(current_proc->fds, fd);
    if (!entry || (entry->flags & FD_FLAG_PATH)) {
        return -EBADF;
    }
    return chmod_fd(entry, mode);
}

/**
 * chmod of the file descriptor `entry` refers to, for fchmod(2), and for
 * fchmodat2(2) with AT_EMPTY_PATH and chmod(2) of /proc/PID/fd/N, which
 * take an O_PATH descriptor too: a KRFFS node or a node of a mounted type
 * by the rules of the path forms, and any other kind as `fchmod_synth`
 * says.
 */
static int32_t chmod_fd(fd_entry_t *entry, uint32_t mode)
{
    if ((entry->type == FD_TYPE_FILE || entry->type == FD_TYPE_DIR) && entry->node) {
        int rc = krffs_chmod_node(entry->node, mode);

        if (rc == 0) {
            attr_told_desc(entry->ofd, IN_ATTRIB, 1);
        }
        return rc;
    }
    if ((entry->type == FD_TYPE_FILE || entry->type == FD_TYPE_DIR) && entry->ofd &&
        entry->ofd->vmnt && entry->ofd->vnode) {
        struct vfs_ref file;
        struct vfs_stat vst;
        int rc;

        file.mnt = entry->ofd->vmnt;
        file.node = entry->ofd->vnode;
        vfs_node_lock(&file);
        rc = vfs_getattr(entry->ofd->vmnt, entry->ofd->vnode, &vst);
        if (rc == 0 && !cred_fs_privileged() && proc_fsuid(current_proc) != vst.uid) {
            rc = -EPERM;
        }
        if (rc == 0) {
            if (!cred_fs_privileged() && !cred_in_group(vst.gid)) {
                mode &= ~02000u;
            }
            vst.mode = mode;
            rc = vfs_setattr(entry->ofd->vmnt, entry->ofd->vnode, VFS_SET_MODE, &vst);
        }
        vfs_node_unlock(&file);
        if (rc == 0) {
            attr_told_desc(entry->ofd, IN_ATTRIB, 0);
        }
        return rc;
    }
    return fchmod_synth(entry, mode);
}

/**
 * The kernel's ids of the owner `*uid` and group `*gid` a chown names in the
 * caller's user namespace (`uid_named`), -1 left as it is. 0, or -EINVAL
 * for one the namespace does not map.
 */
static int32_t chown_ids_named(uint32_t *uid, uint32_t *gid)
{
    int64_t u = *uid == 0xFFFFFFFFu ? (int64_t)*uid : uid_named(*uid);
    int64_t g = *gid == 0xFFFFFFFFu ? (int64_t)*gid : gid_named(*gid);

    if (u < 0 || g < 0) {
        return -EINVAL;
    }
    *uid = (uint32_t)u;
    *gid = (uint32_t)g;
    return 0;
}

/**
 * The path form of chown(2) and lchown(2): change the owner and group of
 * the file at the user's `upath` to `uid` and `gid`, as the caller's user
 * namespace names them, through a final symbolic link when `follow`
 * (`do_chown`); 0 or a negative errno, -EFAULT for a NULL path and -EINVAL
 * for an id the namespace does not map.
 */
static int32_t chown_path(const char __user *upath, uint32_t uid, uint32_t gid, int follow)
{
    char *path;
    int dir;
    int32_t rc;

    if (!current_proc || !upath) {
        return -EFAULT;
    }
    if (chown_ids_named(&uid, &gid) != 0) {
        return -EINVAL;
    }
    rc = path_from_user_dir(AT_FDCWD, upath, &path, &dir);
    if (rc != 0) {
        return rc;
    }
    rc = do_chown(path_name(path), uid, gid, follow, dir);
    path_buf_put(path);
    return rc;
}

/**
 * chown(2): `chown_path` of the path in argument 0 to the ids in arguments
 * 1 and 2, through a final symbolic link. On i386 the 16-bit call, 182,
 * takes 32-bit ids here, as chown32, 212, does.
 */
long sys_chown(isr_params_t *params)
{
    return chown_path((const char __user *)SC_ARG(params, 0), (uint32_t)SC_ARG(params, 1),
                      (uint32_t)SC_ARG(params, 2), 1);
}

/** lchown(2): as chown(2), but a final symbolic link is changed itself,
 * as on Linux. On i386 the 16-bit call, 16, takes 32-bit ids here, as
 * lchown32, 198, does. */
long sys_lchown(isr_params_t *params)
{
    return chown_path((const char __user *)SC_ARG(params, 0), (uint32_t)SC_ARG(params, 1),
                      (uint32_t)SC_ARG(params, 2), 0);
}

/**
 * fchownat(2) takes dirfd, path, uid, gid and flags.
 *
 * The flags are Linux's two, checked first: AT_SYMLINK_NOFOLLOW changes a
 * final symbolic link itself, and AT_EMPTY_PATH with an empty path changes
 * the file `dirfd` is open on, an O_PATH descriptor's included, or the
 * working directory for AT_FDCWD; any other bit is -EINVAL. An empty path
 * without AT_EMPTY_PATH is -ENOENT, and a NULL one -EFAULT.
 */
long sys_fchownat(isr_params_t *params)
{
    int dirfd = (int)SC_ARG(params, 0);
    const char __user *upath = (const char __user *)SC_ARG(params, 1);
    uint32_t uid = (uint32_t)SC_ARG(params, 2);
    uint32_t gid = (uint32_t)SC_ARG(params, 3);
    int flags = (int)SC_ARG(params, 4);
    char *path;
    int empty = 0;
    int dir = 0;
    int32_t rc;

    if (!current_proc) {
        return -ESRCH;
    }
    if (flags & ~(AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH) || chown_ids_named(&uid, &gid) != 0) {
        return -EINVAL;
    }
    if (!upath) {
        return -EFAULT;
    }
    if (flags & AT_EMPTY_PATH) {
        empty = user_path_empty(upath);
        if (empty < 0) {
            return empty;
        }
    }
    if (empty && dirfd != AT_FDCWD) {
        fd_entry_t *entry = fd_get(current_proc->fds, dirfd);

        return entry ? chown_fd(entry, uid, gid) : -EBADF;
    }
    if (empty) {
        path = path_buf_get();
        if (!path) {
            return -ENOMEM;
        }
        k_strncpy(path, proc_cwd(current_proc), VFS_PATH_MAX);
    } else {
        rc = path_from_user_dir(dirfd, upath, &path, &dir);
        if (rc != 0) {
            return rc;
        }
    }
    rc = do_chown(path_name(path), uid, gid, !(flags & AT_SYMLINK_NOFOLLOW), dir);
    path_buf_put(path);
    return rc;
}

/**
 * fchown(2): change the owner and group of what the descriptor in argument
 * 0 is open on to the ids in arguments 1 and 2 (`chown_fd`), under the rule
 * of the path forms; 0 or a negative errno, -EBADF for a descriptor that is
 * not open or was opened with O_PATH, as on Linux, and -EINVAL for an id
 * the caller's user namespace does not map. On i386 the 16-bit call, 95,
 * takes 32-bit ids here, as fchown32, 207, does.
 */
long sys_fchown(isr_params_t *params)
{
    int fd = (int)SC_ARG(params, 0);
    uint32_t uid, gid;
    fd_entry_t *entry;

    if (!current_proc) {
        return -ESRCH;
    }
    entry = fd_get(current_proc->fds, fd);
    if (!entry || (entry->flags & FD_FLAG_PATH)) {
        return -EBADF;
    }
    uid = (uint32_t)SC_ARG(params, 1);
    gid = (uint32_t)SC_ARG(params, 2);
    if (chown_ids_named(&uid, &gid) != 0) {
        return -EINVAL;
    }
    return chown_fd(entry, uid, gid);
}

/**
 * chown_fd of a descriptor on no file of a file system: a /dev or /proc
 * node opened with O_PATH by `chown_synth`, a directory made under /dev by
 * `chown_made`, a pty slave on the pty, -EBADF when the pty is gone, and
 * every other kind with fixed root ownership.
 */
static int32_t chown_fd_synth(fd_entry_t *entry, uint32_t uid, uint32_t gid)
{
    if (entry->type == FD_TYPE_SYNTH_PATH) {
        return entry->priv ? chown_synth((const struct vpath *)entry->priv, uid, gid) : -EBADF;
    }
    if (synth_fd_made(entry)) {
        return chown_made(synth_fd_made(entry), uid, gid);
    }
    if (entry->type == FD_TYPE_DEVICE && devfs_fd_name(entry)) {
        return chown_dev(devfs_fd_name(entry), uid, gid);
    }
    if (entry->type == FD_TYPE_PTS) {
        struct pty *pt = pty_get(pty_slave_index(entry));
        int uid_changes;
        int gid_changes;
        if (!pt) {
            return -EBADF;
        }
        uid_changes = (uid != (uint32_t)-1 && uid != pt->uid);
        gid_changes = (gid != (uint32_t)-1 && gid != pt->gid);
        if ((uid_changes || gid_changes) && !cred_fs_privileged()) {
            return -EPERM;
        }
        if (uid != (uint32_t)-1) {
            pt->uid = uid;
        }
        if (gid != (uint32_t)-1) {
            pt->gid = gid;
        }
        return 0;
    }
    if ((uid != (uint32_t)-1 && uid != 0) || (gid != (uint32_t)-1 && gid != 0)) {
        return -EPERM;
    }
    return 0;
}

/**
 * chown of the file descriptor `entry` refers to, for fchown(2) and for
 * fchownat(2) with AT_EMPTY_PATH, which takes an O_PATH descriptor too: a
 * KRFFS node or a node of a mounted type by the rules of the path forms, and
 * any other kind as `chown_fd_synth` says; fixed root ownership takes a
 * request that changes nothing, and any other is -EPERM. On a mounted type
 * the read, the plan and the store are one step under the mount's namespace
 * lock, as in `do_chown`, and for the same reason.
 */
static int32_t chown_fd(fd_entry_t *entry, uint32_t uid, uint32_t gid)
{
    if ((entry->type == FD_TYPE_FILE || entry->type == FD_TYPE_DIR) && entry->node) {
        int rc = krffs_chown_node(entry->node, uid, gid);

        if (rc == 0 && chown_asks(uid, gid)) {
            attr_told_desc(entry->ofd, IN_ATTRIB, 1);
        }
        return rc;
    }
    if ((entry->type == FD_TYPE_FILE || entry->type == FD_TYPE_DIR) && entry->ofd &&
        entry->ofd->vmnt && entry->ofd->vnode) {
        struct vfs_ref file;
        struct vfs_stat vst;
        int what = 0;
        int rc;

        file.mnt = entry->ofd->vmnt;
        file.node = entry->ofd->vnode;
        vfs_node_lock(&file);
        rc = vfs_getattr(entry->ofd->vmnt, entry->ofd->vnode, &vst);
        if (rc == 0) {
            what = vfs_chown_plan(&vst, uid, gid);
            rc = what < 0
                     ? what
                     : (what ? vfs_setattr(entry->ofd->vmnt, entry->ofd->vnode, what, &vst) : 0);
        }
        vfs_node_unlock(&file);
        if (rc == 0 && chown_asks(uid, gid)) {
            attr_told_desc(entry->ofd, IN_ATTRIB, what == 0);
        }
        return rc;
    }
    return chown_fd_synth(entry, uid, gid);
}

/**
 * The refusals of truncate(2) for the file `ref` resolved to. An immutable
 * or append-only file of a mounted type is -EPERM before anything else;
 * then, in Linux's order, a directory is -EISDIR and anything but a regular
 * file -EINVAL, a read-only mount -EROFS, a caller without write permission
 * -EACCES, and a file a loop device holds -EBUSY. A /proc file is -EACCES
 * to every caller, root included, as an open of one for writing is.
 * Returns 0 or the errno.
 */
static int32_t truncate_checks(const struct vfs_ref *ref)
{
    const struct vpath *vp = vfs_synth_vpath(ref->mnt, ref->node);
    krffs_node_t *node = ref->node;
    uint32_t mode;
    int32_t rc;

    if (vp) {
        struct kstat kst;

        rc = synth_stat(vp, &kst, 1);
        if (rc != 0) {
            return rc;
        }
        mode = kst.st_mode;
    } else if (mnt_is_root(ref->mnt)) {
        mode = node == krffs_fs->node ? S_IFDIR : krffs_mode_type(node->mode);
    } else {
        struct vfs_stat vst;

        rc = vfs_getattr(ref->mnt, ref->node, &vst);
        if (rc != 0) {
            return rc;
        }
        if (vst.flags_known && (vst.flags & (VFS_FL_IMMUTABLE | VFS_FL_APPEND))) {
            return -EPERM;
        }
        mode = vst.mode;
    }
    if ((mode & S_IFMT) == S_IFDIR) {
        return -EISDIR;
    }
    if ((mode & S_IFMT) != S_IFREG) {
        return -EINVAL;
    }
    if (vp) {
        return -EACCES;
    }
    if (ref->mnt->flags & VFS_MS_RDONLY) {
        return -EROFS;
    }
    if (mnt_is_root(ref->mnt)) {
        rc = cred_access_node(node, CRED_W, 0);
    } else {
        rc = cred_access_ref(ref, CRED_W, 0);
    }
    if (rc != 0) {
        return rc;
    }
    return loop_backs(ref->mnt, ref->node) ? -EBUSY : 0;
}

/**
 * Set the length of the file at `path`, an absolute path in a path page, to
 * `length` bytes, which is not negative, for truncate(2) and truncate64(2);
 * 0 or a negative errno.
 *
 * The path is resolved through links, to a directory when it named one by
 * its end, `dir` (`resolve_named`), and `truncate_checks` has what is
 * refused. The file is then truncated through an open file description of
 * its own, which no descriptor points at, with the open, the lock and the
 * close of an open for writing and an ftruncate: a KRFFS file that must move
 * to grow repoints the description, so the set-id clear of an unprivileged
 * caller finds the file where it is now, and a loop attachment sees the
 * truncation in progress, as it sees an ftruncate. A length that grows the
 * file past the caller's RLIMIT_FSIZE is -EFBIG with SIGXFSZ, judged under
 * the file's lock, or on FAT32 and exFAT the namespace lock
 * (`fsize_growth_refused`); a shrink is never refused. A
 * file reserved for execution gives -ETXTBSY, as on Linux.
 */
static int32_t truncate_path(const char *path, long long length, int dir)
{
    struct vfs_ref ref, file;
    struct vfs_mount *locked;
    struct open_file *desc = NULL;
    int32_t rc;
    int own, ns;

    rc = resolve_named(path_name(path), VFS_WALK_LOCK, dir, &ref);
    if (rc != 0) {
        return rc;
    }
    locked = ref.mnt;
    rc = truncate_checks(&ref);
    if (rc == 0) {
        desc = fd_ofd_new();
        rc = desc ? fd_ofd_access(desc, &ref, 1) : -ENOMEM;
        if (rc == 0) {
            rc = vfs_open_node(ref.mnt, ref.node, 1 /* O_WRONLY */, &desc->fhandle);
        }
    }
    if (rc != 0) {
        fd_ofd_unref(desc);
        vfs_walk_unlock(locked);
        vfs_ref_release(&ref);
        return rc;
    }
    desc->vmnt = ref.mnt;
    vfs_wb_err_sample(ref.mnt, &desc->wb_seen, &desc->sb_seen);
    desc->vnode = ref.node;
    vfs_desc_open(ref.mnt);
    vfs_leave(ref.mnt);
    fd_ofd_lock(desc);
    own = file_lock(desc, &file);
    ns = size_ns_lock(desc);
    rc = -EBUSY;
    if (!loop_backs(desc->vmnt, desc->vnode)) {
        rc = fsize_growth_refused(desc, (uint64_t)length);
    }
    if (rc == 0) {
        rc = vfs_truncate(desc->vmnt, desc->fhandle, (uint64_t)length);
    }
    size_ns_unlock(desc, ns);
    if (rc == 0 && !cred_fs_privileged()) {
        if (mnt_is_root(desc->vmnt)) {
            cred_kill_setid(desc->vnode);
        } else {
            vfs_kill_setid(desc->vmnt, desc->vnode);
        }
    }
    if (rc == 0) {
        struct vfs_stat st;

        if (vfs_getattr(desc->vmnt, desc->vnode, &st) == 0) {
            filemap_file_truncated(desc->vmnt, st.ino, (uint64_t)length);
            if (inotify_watches) {
                inotify_file(desc->vmnt, st.ino, IN_MODIFY);
            }
        }
    }
    file_unlock(&file, own);
    fd_ofd_unlock(desc);
    fd_ofd_unref(desc);
    vfs_walk_unlock(locked);
    return rc;
}

long ksys_truncate(const char __user *upath, long long length)
{
    char *path;
    int dir;
    int32_t rc;

    if (!current_proc) {
        return -ESRCH;
    }
    if (length < 0) {
        return -EINVAL;
    }
    if (!upath) {
        return -EFAULT;
    }
    rc = path_from_user_dir(AT_FDCWD, upath, &path, &dir);
    if (rc != 0) {
        return rc;
    }
    rc = truncate_path(path, length, dir);
    path_buf_put(path);
    return rc;
}

/** truncate(2) takes path and the length, a signed register's worth, so of
 * 32 bits on i686, whose truncate64 is in arch/i686/sys_ia32.c. */
long sys_truncate(isr_params_t *params)
{
    return ksys_truncate((const char __user *)SC_ARG(params, 0), SC_SIGNED(params, 1));
}

/**
 * Walk to the parent of the new name `path`, with or without its leading
 * '/', for a call that makes a node other than a directory there:
 * `vfs_resolve` with VFS_WALK_PARENT, the last name in `leaf` of `leafcap`
 * bytes, and on success `parent` held for the caller to release. The root,
 * which exists and has no last name to make, is -EEXIST, as Linux answers
 * link(2), symlink(2) and mknod(2) there, and bind(2) as EADDRINUSE.
 */
static int32_t new_name_parent(const char *path, struct vfs_ref *parent, char *leaf,
                               uint32_t leafcap)
{
    if (path[0] == '\0' || (path[0] == '/' && path[1] == '\0')) {
        return -EEXIST;
    }
    return vfs_resolve(0, path, VFS_WALK_PARENT, parent, leaf, leafcap);
}

/**
 * Whether `name` is in directory `dir` of `mnt`: 1 or 0, or a negative errno
 * for a lookup that failed another way. The node a lookup takes goes back.
 */
static int name_present(struct vfs_mount *mnt, void *dir, const char *name)
{
    void *found;
    int rc = mnt->type->ops->lookup(mnt, dir, name, &found);

    if (rc == -ENOENT) {
        return 0;
    }
    if (rc != 0) {
        return rc;
    }
    if (mnt->type->ops->put) {
        mnt->type->ops->put(mnt, found);
    }
    return 1;
}

/**
 * Whether the name the path `path` ends in, in a directory of /dev, /proc or
 * /sys, exists: -EEXIST when it does, a final link not followed, and the
 * walk's error, -ENOENT for a name the namespace does not have, when it does
 * not. The synthetic types look whole paths up, so the name is looked up by
 * `path`, and a device name they classify exists only when `synth_stat`
 * finds its device; any other failure of `synth_stat` is of a name that
 * exists.
 */
static int32_t synth_name_exists(const char *path)
{
    struct kstat kst;
    const struct vpath *vp;
    struct vfs_ref ref;
    int32_t rc = vfs_resolve(0, path, VFS_WALK_NOFOLLOW, &ref, 0, 0);

    if (rc != 0) {
        return rc;
    }
    vp = vfs_synth_vpath(ref.mnt, ref.node);
    rc = vp ? synth_stat(vp, &kst, 0) : 0;
    vfs_ref_release(&ref);
    return rc == -ENOENT ? rc : -EEXIST;
}

/**
 * The answer of a call that makes a node other than a directory, `leaf` in
 * the held directory `parent`, the last name of `path`, when `path` named a
 * directory by its end, `end` (`path_names_dir`), as Linux answers it before
 * a read-only mount's EROFS: -EEXIST for a last "." or "..", which exists,
 * and after a slash -EEXIST for a name that exists, a mount point among
 * them, and the lookup's -ENOENT for one that does not, in /dev, /proc and
 * /sys too (`synth_name_exists`), where the call's -EROFS comes after. 0 for
 * any other path.
 */
static int32_t make_dir_end(const struct vfs_ref *parent, const char *leaf, int end,
                            const char *path)
{
    void *found;
    int rc;

    if (leaf_is_dot(leaf)) {
        return -EEXIST;
    }
    if (end != PATH_END_SLASH) {
        return 0;
    }
    if (vfs_synth_vpath(parent->mnt, parent->node)) {
        return synth_name_exists(path);
    }
    if (vfs_leaf_mount(parent->mnt, parent->node, leaf)) {
        return -EEXIST;
    }
    rc = parent->mnt->type->ops->lookup(parent->mnt, parent->node, leaf, &found);
    if (rc != 0) {
        return rc;
    }
    if (parent->mnt->type->ops->put) {
        parent->mnt->type->ops->put(parent->mnt, found);
    }
    return -EEXIST;
}

/**
 * Whether the new name `leaf` in the held directory `parent`, the last name
 * of `path`, is taken, for a call that makes a node there: -EEXIST when a
 * node has it, or a mount stands at it, whatever the type's own lookup says
 * (a KRFFS /dev would shadow the synthetic mount), 0 when nothing does, or
 * the lookup's error. Linux's walk to a new name answers this first, so a
 * name that exists is -EEXIST before the refusals of a read-only mount, of
 * /dev, /proc and /sys, of another mount, of a file system without the
 * kind, and of the write permission. A directory of /dev, /proc or /sys has
 * the name looked up by `path` (`synth_name_exists`).
 */
static int32_t new_name_taken(const struct vfs_ref *parent, const char *leaf, const char *path)
{
    int32_t rc;

    if (vfs_synth_vpath(parent->mnt, parent->node)) {
        rc = synth_name_exists(path);
        return rc == -ENOENT ? 0 : rc;
    }
    if (vfs_leaf_mount(parent->mnt, parent->node, leaf)) {
        return -EEXIST;
    }
    rc = name_present(parent->mnt, parent->node, leaf);
    return rc == 1 ? -EEXIST : rc;
}

/**
 * Whether the caller may make a hard link to the file `src` of attributes
 * `st`, as Linux's may_linkat (fs/namei.c:1198-1218): 0, or -EPERM under
 * fs.protected_hardlinks for a file the caller neither owns nor may act as
 * owner of (CAP_FOWNER) that is not a regular file, is setuid, is setgid
 * and group-executable, or that the caller may not both read and write. A
 * user who could link /etc/shadow into a directory of its own could leave a
 * name there that a root process later acts on, and could keep a setuid
 * program's old version alive past an upgrade.
 */
static int32_t may_linkat(const struct vfs_ref *src, const struct vfs_stat *st)
{
    if (!vfs_protected_hardlinks || st->uid == proc_fsuid(current_proc) || cred_fs_privileged()) {
        return 0;
    }
    if ((st->mode & S_IFMT) == S_IFREG && !(st->mode & 04000u) && (st->mode & 02010u) != 02010u &&
        cred_access_ref(src, CRED_R | CRED_W, 0) == 0) {
        return 0;
    }
    return -EPERM;
}

/**
 * Make the hard link `newname` to `oldname`, both canonical paths: the core
 * of link(2) and linkat(2). Returns 0 or a negative errno.
 *
 * The source is resolved as itself unless `follow`; one that named a
 * directory by its end, `olddir`, follows a final link and must reach a
 * directory (`resolve_named`), which is EPERM later. The refusals after the
 * walks come in this order: the root as the new name, EEXIST
 * (`new_name_parent`); a new name that named a directory by its end,
 * `newend`, as `make_dir_end` answers it; a new name that exists, EEXIST
 * (`new_name_taken`); another mount, EXDEV; a source in /dev, /proc or /sys
 * or on the KRFFS root, whose types have no links, and a directory, EPERM;
 * a read-only mount, EROFS; fs.protected_hardlinks, EPERM (`may_linkat`);
 * and the parent's write permission. The type
 * makes the link under the parent's lock (`vfs_node_lock`), and under the
 * source's after it on a type that sets `file_locked`, and one without links
 * answers EPERM.
 */
static int32_t do_link(const char *oldname, const char *newname, int follow, int olddir, int newend)
{
    struct vfs_ref src, parent;
    struct vfs_stat st;
    char leaf[VFS_NAME_MAX + 1];
    int32_t rc;

    rc = resolve_named(oldname, follow ? 0 : VFS_WALK_NOFOLLOW, olddir, &src);
    if (rc != 0) {
        return rc;
    }
    rc = new_name_parent(newname, &parent, leaf, sizeof(leaf));
    if (rc != 0) {
        vfs_ref_release(&src);
        return rc;
    }
    rc = make_dir_end(&parent, leaf, newend, newname);
    if (rc == 0) {
        rc = new_name_taken(&parent, leaf, newname);
    }
    if (rc != 0) {
        goto out;
    }
    if (parent.mnt != src.mnt) {
        rc = -EXDEV;
    } else if (vfs_synth_vpath(src.mnt, src.node) || mnt_is_root(src.mnt)) {
        rc = -EPERM;
    } else {
        rc = vfs_getattr(src.mnt, src.node, &st);
    }
    if (rc == 0 && (st.mode & 0170000u) == 0040000u) {
        rc = -EPERM;
    } else if (rc == 0 && (parent.mnt->flags & VFS_MS_RDONLY)) {
        rc = -EROFS;
    } else if (rc == 0) {
        rc = may_linkat(&src, &st);
    }
    if (rc == 0) {
        rc = (int32_t)cred_access_ref(&parent, CRED_W, 0);
    }
    if (rc == 0) {
        int own = vfs_file_locked(parent.mnt);

        rc = dir_lock(&parent, leaf, RECHECK_WRITE | RECHECK_NEW);
        if (own) {
            sleep_lock_subclass(2);
            vfs_node_lock(&src);
            sleep_lock_subclass(0);
        }
        if (rc == 0) {
            rc = vfs_link(parent.mnt, parent.node, leaf, src.node);
        }
        if (own) {
            vfs_node_unlock(&src);
        }
        vfs_node_unlock(&parent);
    }
out:
    vfs_ref_release(&parent);
    vfs_ref_release(&src);
    return rc;
}

/**
 * Make the symbolic link `newname`, a canonical path, to `target`, which is
 * stored as it is: the core of symlink(2) and symlinkat(2). Returns 0 or a
 * negative errno: -ENOENT for an empty target, as on Linux; -EEXIST for the
 * root as the new name (`new_name_parent`); for a new name that named a
 * directory by its end, `end`, what `make_dir_end` answers; -EEXIST for a
 * new name that exists (`new_name_taken`); then a directory of /dev takes the
 * link (`synth_dev_make`), and the rest of /dev, /proc and /sys and a
 * read-only mount are -EROFS, -EPERM on the KRFFS root and on a type
 * without symbolic links, and the directory's write permission. The type
 * makes the link under the directory's lock (`dir_lock`).
 */
static int32_t do_symlink(const char *target, const char *newname, int end)
{
    struct vfs_ref parent;
    char leaf[VFS_NAME_MAX + 1];
    int32_t rc;

    if (target[0] == '\0') {
        return -ENOENT;
    }
    rc = new_name_parent(newname, &parent, leaf, sizeof(leaf));
    if (rc != 0) {
        return rc;
    }
    rc = make_dir_end(&parent, leaf, end, newname);
    if (rc == 0) {
        rc = new_name_taken(&parent, leaf, newname);
    }
    if (rc != 0) {
        vfs_ref_release(&parent);
        return rc;
    }
    if (vfs_synth_vpath(parent.mnt, parent.node)) {
        rc = synth_dev_make(&parent, leaf, 0777u, target);
        vfs_ref_release(&parent);
        return rc;
    }
    if (parent.mnt->flags & VFS_MS_RDONLY) {
        rc = -EROFS;
    } else if (mnt_is_root(parent.mnt) || !parent.mnt->type->ops->symlink) {
        rc = -EPERM;
    } else {
        rc = (int32_t)cred_access_ref(&parent, CRED_W, 0);
    }
    if (rc == 0) {
        rc = dir_lock(&parent, leaf, RECHECK_WRITE | RECHECK_NEW);
        if (rc == 0) {
            rc = vfs_symlink(parent.mnt, parent.node, leaf, target);
        }
        vfs_node_unlock(&parent);
    }
    vfs_ref_release(&parent);
    return rc;
}

/**
 * symlink(2) and symlinkat(2) after their arguments: the target is copied as
 * it is, up to `VFS_PATH_MAX` bytes with its terminator, and never resolved;
 * the new name is resolved from `newdirfd`. Returns 0 or a negative errno:
 * -ENOMEM with no path page, the copies' errors, -ENOENT for an empty target
 * among them, as on Linux, or `do_symlink`'s.
 */
static int32_t symlink_from_user(const char __user *utarget, int newdirfd, const char __user *unew)
{
    char *target = path_buf_get();
    char *newpath = NULL;
    int end = 0;
    int32_t rc;

    if (!target) {
        return -ENOMEM;
    }
    rc = strncpy_from_user(target, utarget, VFS_PATH_MAX);
    if (rc == 0) {
        rc = -ENOENT;
    }
    if (rc > 0) {
        rc = path_from_user_dir(newdirfd, unew, &newpath, &end);
    }
    if (rc == 0) {
        rc = do_symlink(target, path_name(newpath), end);
    }
    path_buf_put(newpath);
    path_buf_put(target);
    return rc;
}

/**
 * symlink(2): the target and the new name, which is resolved from the
 * working directory. -EFAULT for a NULL pointer, or `symlink_from_user`'s
 * answer.
 */
long sys_symlink(isr_params_t *params)
{
    const char __user *utarget = (const char __user *)SC_ARG(params, 0);
    const char __user *unew = (const char __user *)SC_ARG(params, 1);

    if (!current_proc || !utarget || !unew) {
        return -EFAULT;
    }
    return symlink_from_user(utarget, AT_FDCWD, unew);
}

/**
 * symlinkat(2): the target, the directory descriptor and the new name, which
 * is resolved from `newdirfd` when it is relative. -EFAULT for a NULL
 * pointer, or `symlink_from_user`'s answer.
 */
long sys_symlinkat(isr_params_t *params)
{
    const char __user *utarget = (const char __user *)SC_ARG(params, 0);
    int newdirfd = (int)SC_ARG(params, 1);
    const char __user *unew = (const char __user *)SC_ARG(params, 2);

    if (!current_proc || !utarget || !unew) {
        return -EFAULT;
    }
    return symlink_from_user(utarget, newdirfd, unew);
}

/**
 * link(2) and linkat(2) after their arguments: both paths are copied, which
 * reports a bad pointer as -EFAULT and a bad directory descriptor as -EBADF,
 * before the source is looked up, so a missing source is -ENOENT and not the
 * -EPERM of a file system without links. `follow` has a final link of the
 * source followed. Returns 0 or a negative errno, `do_link`'s after the
 * copies.
 */
static int32_t link_from_user(int olddirfd, const char __user *uold, int newdirfd,
                              const char __user *unew, int follow)
{
    char *oldpath = NULL;
    char *newpath = NULL;
    int olddir = 0;
    int newend = 0;
    int32_t rc = path_from_user_dir(olddirfd, uold, &oldpath, &olddir);

    if (rc == 0) {
        rc = path_from_user_dir(newdirfd, unew, &newpath, &newend);
    }
    if (rc == 0) {
        rc = do_link(path_name(oldpath), path_name(newpath), follow, olddir, newend);
    }
    path_buf_put(newpath);
    path_buf_put(oldpath);
    return rc;
}

/**
 * link(2): the old and the new path, each resolved from the working
 * directory; a final link of the old path is not followed, as on Linux.
 * -EFAULT for a NULL pointer, or `link_from_user`'s answer.
 */
long sys_link(isr_params_t *params)
{
    const char __user *uold = (const char __user *)SC_ARG(params, 0);
    const char __user *unew = (const char __user *)SC_ARG(params, 1);

    if (!current_proc || !uold || !unew) {
        return -EFAULT;
    }
    return link_from_user(AT_FDCWD, uold, AT_FDCWD, unew, 0);
}

/**
 * linkat(2): the old directory descriptor and path, the new ones, and the
 * flags. AT_SYMLINK_FOLLOW has a final link of the old path followed;
 * AT_EMPTY_PATH is taken and changes nothing, so an empty old path is
 * -ENOENT; any other flag is -EINVAL. -EFAULT for a NULL pointer, or
 * `link_from_user`'s answer.
 */
long sys_linkat(isr_params_t *params)
{
    int olddirfd = (int)SC_ARG(params, 0);
    const char __user *uold = (const char __user *)SC_ARG(params, 1);
    int newdirfd = (int)SC_ARG(params, 2);
    const char __user *unew = (const char __user *)SC_ARG(params, 3);
    int flags = (int)SC_ARG(params, 4);

    if (!current_proc || !uold || !unew) {
        return -EFAULT;
    }
    if (flags & ~(AT_SYMLINK_FOLLOW | AT_EMPTY_PATH)) {
        return -EINVAL;
    }
    return link_from_user(olddirfd, uold, newdirfd, unew, (flags & AT_SYMLINK_FOLLOW) != 0);
}

/** The 64-bit displacement `_llseek` takes as two words, high and low. */
static long long off_from(uint32_t high, uint32_t low)
{
    return (long long)(((uint64_t)high << 32) | (uint64_t)low);
}

/**
 * `_llseek` on a raw disk window, by `raw_seek_resolve`'s rules, under the
 * description's lock: -ENXIO when the window's size cannot be read,
 * -EOVERFLOW for a place past the largest offset and -EFAULT with the offset
 * unchanged when `result` is NULL, and with it moved when the copy fails.
 * Returns 0 or a negative errno.
 */
static int32_t llseek_raw(fd_entry_t *entry, long long off, int whence, long long __user *result)
{
    uint64_t raw_size, resolved;
    long long new_off = 0;
    int32_t rc;

    fd_ofd_lock(entry->ofd);
    if (!devfs_fd_raw_size(entry, &raw_size)) {
        fd_ofd_unlock(entry->ofd);
        return -ENXIO;
    }
    rc = raw_seek_resolve(entry->ofd->raw_offset, raw_size, off, whence, &resolved);
    if (rc == 0 && resolved > (uint64_t)0x7FFFFFFFFFFFFFFFull) {
        rc = -EOVERFLOW;
    }
    if (rc == 0 && !result) {
        rc = -EFAULT;
    }
    if (rc == 0) {
        new_off = (long long)resolved;
        entry->ofd->raw_offset = resolved;
    }
    fd_ofd_unlock(entry->ofd);
    if (rc != 0) {
        return rc;
    }
    return copy_to_user(result, &new_off, sizeof(new_off)) != 0 ? -EFAULT : 0;
}

/**
 * `_llseek` on /dev/null, /dev/zero or /dev/full, which land at 0 whatever
 * the displacement. A NULL `result` is allowed; a bad one is -EFAULT.
 */
static int32_t llseek_zero(fd_entry_t *entry, long long __user *result)
{
    long long zero = 0;

    entry->ofd->offset = 0;
    if (result && copy_to_user(result, &zero, sizeof(zero)) != 0) {
        return -EFAULT;
    }
    return 0;
}

/**
 * `_llseek` on `/dev/bootlog`, by `seek_bootlog`'s rules, with the new offset
 * written to the caller's `result`: -EFAULT with the offset unchanged when
 * `result` is NULL, and -EFAULT with it moved when the copy fails. Returns 0
 * or a negative errno.
 */
static int32_t llseek_bootlog(fd_entry_t *entry, long long off, int whence,
                              long long __user *result)
{
    long long at;
    int rc = seek_bootlog(entry, off, whence, 0xFFFFFFFFull, result != NULL, &at);

    if (rc == 0 && !result) {
        rc = -EFAULT;
    }
    if (rc != 0) {
        return rc;
    }
    return copy_to_user(result, &at, sizeof(at)) != 0 ? -EFAULT : 0;
}

/**
 * `_llseek` on a directory or a /proc file, by `seek_listing`'s rules, with
 * the new offset written to the caller's `result`, which must not be NULL.
 * Returns 0 or a negative errno.
 */
static int32_t llseek_listing(fd_entry_t *entry, long long off, int whence,
                              long long __user *result)
{
    long long at;
    int rc;

    if (!result) {
        return -EFAULT;
    }
    rc = seek_listing(entry, off, whence, 0x7FFFFFFFFFFFFFFFull, &at);
    if (rc == 0 && copy_to_user(result, &at, sizeof(at)) != 0) {
        rc = -EFAULT;
    }
    return rc;
}

/**
 * `_llseek` on /fb, by `seek_framebuffer`'s rules, with the new offset
 * written to the caller's `result`, which must not be NULL. Returns 0 or a
 * negative errno.
 */
static int32_t llseek_framebuffer(fd_entry_t *entry, long long off, int whence,
                                  long long __user *result)
{
    long long at;
    int rc;

    if (!result) {
        return -EFAULT;
    }
    rc = seek_framebuffer(entry, off, whence, 0x7FFFFFFFFFFFFFFFull, &at);
    if (rc == 0 && copy_to_user(result, &at, sizeof(at)) != 0) {
        rc = -EFAULT;
    }
    return rc;
}

/**
 * The current size of the file an ordinary descriptor is open on, into
 * `*out`: a KRFFS root file's from its node, and another type's from its
 * file system.
 *
 * A `SEEK_END` resolves against this, so the caller reads it inside the
 * description's lock: a process sharing the description can extend the
 * file while this one waits.
 *
 * @return 0; -EBADF for a KRFFS root file whose node was unlinked or moved
 * from under the descriptor (`fd_node_check`) and for a descriptor with no
 * file; or the file system's error for its size.
 */
static int file_size_now(fd_entry_t *entry, uint64_t *out)
{
    if (entry->node) {
        if (fd_node_check(entry) != 0) {
            return -EBADF;
        }
        *out = entry->node->data_size;
        return 0;
    }
    if (entry->ofd->vmnt) {
        struct vfs_stat vst;
        int rc = vfs_getattr(entry->ofd->vmnt, entry->ofd->vnode, &vst);

        if (rc != 0) {
            return rc;
        }
        *out = vst.size;
        return 0;
    }
    return -EBADF;
}

/** Seek a devfs descriptor and copy its 64-bit position to the caller. */
static int32_t device_llseek(fd_entry_t *entry, long long off, int whence, long long __user *result)
{
    if (devfs_fd_is_raw(entry)) {
        return llseek_raw(entry, off, whence, result);
    }
    if (devfs_fd_log_size(entry, NULL)) {
        return llseek_bootlog(entry, off, whence, result);
    }
    if (devfs_lseek_zero(entry)) {
        return llseek_zero(entry, result);
    }
    return -ESPIPE;
}

/**
 * _llseek(2), i386's 64-bit lseek: the descriptor, the high and low words of
 * the displacement, the address of the 64-bit result and `whence`. Moves the
 * offset and writes it to `*result`. Returns 0 or a negative errno: -EBADF
 * for a descriptor that is not open or is an O_PATH one; -ESPIPE for any
 * other descriptor that does not seek, among them a pipe, a socket, a
 * terminal, the mouse and a device; -ENXIO for a disk window whose size
 * cannot be read; -EINVAL for a bad `whence` or a place before 0; -ENXIO
 * for SEEK_DATA or SEEK_HOLE at or past the end of a file;
 * -EOVERFLOW for a place past the largest offset; -EFAULT for a NULL or bad
 * `result`.
 *
 * A raw disk window seeks by `llseek_raw`, the log by `llseek_bootlog`, /fb
 * by `llseek_framebuffer`, a directory or a /proc file by `llseek_listing`,
 * and /dev/null, /dev/zero and /dev/full land at 0 and take a NULL `result`.
 * A file's offset moves under the description's lock, as read and write move
 * it: every descriptor a dup or a fork made shares the offset, and a seek
 * from the current place that read it outside the lock would start from a
 * place another process had already left. SEEK_END reads the size again
 * inside the lock, since another process on the description may extend the
 * file while this call waits for it, and so do SEEK_DATA and SEEK_HOLE
 * (`seek_data_hole`). A NULL `result` is -EFAULT for a file; musl always
 * passes one.
 */
static long llseek_call(isr_params_t *params)
{
    int fd = (int)SC_ARG(params, 0);
    uint32_t offset_high = (uint32_t)SC_ARG(params, 1);
    uint32_t offset_low = (uint32_t)SC_ARG(params, 2);
    long long __user *result = (long long __user *)SC_ARG(params, 3);
    int whence = (int)SC_ARG(params, 4);
    fd_entry_t *entry;
    long long off, new_off;
    uint64_t size = 0;
    int seek_rc;

    if (!current_proc) {
        return -ESRCH;
    }
    entry = fd_get(current_proc->fds, fd);
    if (!entry || (entry->flags & FD_FLAG_PATH)) {
        return -EBADF;
    }
    if (entry->ops && entry->ops->llseek) {
        new_off = entry->ops->llseek(entry, off_from(offset_high, offset_low), whence);
        if (new_off < 0)
            return (long)new_off;
        if (copy_to_user(result, &new_off, sizeof(new_off)))
            return -EFAULT;
        entry->ofd->offset = (uint64_t)new_off;
        return 0;
    }
    if (entry->type == FD_TYPE_PIPE) {
        return -ESPIPE; /* a pipe is not seekable (POSIX) */
    }
    if (entry->type == FD_TYPE_DEVICE) {
        return device_llseek(entry, off_from(offset_high, offset_low), whence, result);
    }
    if (entry->type == FD_TYPE_CONSOLE || entry->type == FD_TYPE_MOUSE) {
        return -ESPIPE;
    }
    if (entry->type == FD_TYPE_FRAMEBUFFER && entry->ofd) {
        return llseek_framebuffer(entry, off_from(offset_high, offset_low), whence, result);
    }
    if (seeks_listing(entry)) {
        return llseek_listing(entry, off_from(offset_high, offset_low), whence, result);
    }
    if (entry->type != FD_TYPE_FILE) {
        return -ESPIPE;
    }
    seek_rc = entry->ofd ? file_size_now(entry, &size) : -EBADF;
    if (seek_rc != 0) {
        return seek_rc;
    }

    if (!result) {
        return -EFAULT;
    }
    off = (long long)(((uint64_t)offset_high << 32) | (uint64_t)offset_low);
    fd_ofd_lock(entry->ofd);
    if (whence >= 2) {
        seek_rc = file_size_now(entry, &size);
        if (seek_rc != 0) {
            fd_ofd_unlock(entry->ofd);
            return seek_rc;
        }
    }
    switch (whence) {
    case 0: /* SEEK_SET */
        new_off = off;
        seek_rc = 0;
        break;
    case 1: /* SEEK_CUR */
        seek_rc = seek_add(entry->ofd->offset, off, &new_off);
        break;
    case 2: /* SEEK_END */
        seek_rc = seek_add(size, off, &new_off);
        break;
    case 3: /* SEEK_DATA */
    case 4: /* SEEK_HOLE */
        seek_rc = seek_data_hole(size, off, whence, &new_off);
        break;
    default:
        fd_ofd_unlock(entry->ofd);
        return -EINVAL;
    }
    if (seek_rc != 0) {
        fd_ofd_unlock(entry->ofd);
        return seek_rc;
    }
    if (new_off < 0) {
        fd_ofd_unlock(entry->ofd);
        return -EINVAL;
    }
    entry->ofd->offset = (uint64_t)new_off;
    fd_ofd_unlock(entry->ofd);
    if (copy_to_user(result, &new_off, sizeof(new_off)) != 0) {
        return -EFAULT;
    }
    return 0;
}

/** _llseek(2): `llseek_call`, under the big lock for a descriptor that
 * `seek_unlocked` does not admit. */
long sys__llseek(isr_params_t *params)
{
    long rc;

    if (!current_proc || bkl_held() ||
        seek_unlocked(fd_get(current_proc->fds, (int)SC_ARG(params, 0)))) {
        return llseek_call(params);
    }
    bkl_enter();
    rc = llseek_call(params);
    bkl_exit();
    return rc;
}

/**
 * The refusals pread64 and pwrite64 share, in Linux's order: a negative
 * offset is -EINVAL before the descriptor is looked at, a descriptor that is
 * not open or is an O_PATH one is -EBADF, one that cannot seek is -ESPIPE,
 * one not open for the direction (`write` or not) is -EBADF, one with no
 * method for it is -EINVAL, a buffer outside user memory is -EFAULT, a
 * count or an end past the signed range is -EINVAL (`rw_end_past`), and a
 * directory is -EISDIR. The buffer and count are the original request,
 * before its cut.
 *
 * What cannot seek is `pio_seekable`'s list. A /proc snapshot is read at an
 * offset, as Linux reads one, the framebuffer written at one, and an
 * anonymous object with a position, a VFIO device's, both.
 * `entry` is the caller's lookup, NULL for none.
 * Returns 0 with the descriptor in `*out`, or the errno.
 */
static int32_t pio_check(fd_entry_t *entry, int write, uintptr_t buf, size_t count, long long pos,
                         fd_entry_t **out)
{
    int32_t rc;

    if (pos < 0) {
        return -EINVAL;
    }
    if (!current_proc) {
        return -ESRCH;
    }
    if (!entry || (entry->flags & FD_FLAG_PATH)) {
        return -EBADF;
    }
    if (!pio_seekable(entry)) {
        return -ESPIPE;
    }
    rc = write ? write_allowed(entry) : read_allowed(entry);
    if (rc != 0) {
        return rc;
    }
    rc = rw_buffer_range(buf, count);
    if (rc != 0) {
        return rc;
    }
    if (rw_end_past(entry, (uint64_t)pos, (uint64_t)count)) {
        return -EINVAL;
    }
    if (entry->type == FD_TYPE_DIR || entry->type == FD_TYPE_SYNTH_DIR) {
        return -EISDIR;
    }
    *out = entry;
    return 0;
}

/** `pio_check` for descriptor `fd`, which it looks up. */
static int32_t pio_entry(int fd, int write, uintptr_t buf, size_t count, long long pos,
                         fd_entry_t **out)
{
    return pio_check(current_proc ? fd_get(current_proc->fds, fd) : NULL, write, buf, count, pos,
                     out);
}

/**
 * Make `view` a copy of the device or /proc descriptor `entry` whose
 * description is `desc`, a copy of the shared one with both offsets at `at`,
 * for pread64 and pwrite64.
 *
 * A device's read or write moves the offset it uses, a raw window's own or
 * the description's for the log, a /proc snapshot, /proc/PID/mem and the
 * framebuffer, and reads nothing else of the description. Given the copy, it
 * uses and moves the copy's, and the shared description, which a sharer may
 * be reading or seeking at the same time, never changes. The offset is the
 * whole position, since one of /proc/PID/mem is an address, past 4 GiB on
 * x86-64; the log and a snapshot are shorter than 4 GiB, and take a position
 * past that as their end.
 */
static void pio_view(fd_entry_t *entry, uint64_t at, fd_entry_t *view, struct open_file *desc)
{
    *desc = *entry->ofd;
    desc->offset = at;
    desc->raw_offset = at;
    *view = *entry;
    view->ofd = desc;
}

/**
 * A regular file is read at the offset under the description's lock, as read(2)
 * reads it, and a device, a /proc snapshot or a debugfs file, whose
 * operations read, through `pio_view`. `e` is the caller's lookup.
 */
static long pread_at(fd_entry_t *e, char __user *buf, size_t requested, long long pos)
{
    uint64_t at = (uint64_t)pos;
    fd_entry_t *entry = NULL;
    fd_entry_t view;
    struct open_file desc;
    int32_t rc = pio_check(e, 0, (__force uintptr_t)buf, requested, pos, &entry);
    uint32_t count = USER_RW_LEN(requested);

    if (rc != 0 || count == 0) {
        return rc;
    }
    if (entry->type == FD_TYPE_FILE && !entry->ops) {
        return file_read(entry, buf, count, &at, count);
    }
    if (!entry->ops || !entry->ops->read || !entry->ofd) {
        return -EBADF;
    }
    pio_view(entry, at, &view, &desc);
    return entry->ops->read(&view, buf, count);
}

/** Write at the offset through file_write or a device's private view,
 * after checking the original request as pread64 does. `e` is the caller's
 * lookup. */
static long pwrite_at(fd_entry_t *e, const char __user *buf, size_t requested, long long pos)
{
    uint64_t at = (uint64_t)pos;
    fd_entry_t *entry = NULL;
    fd_entry_t view;
    struct open_file desc;
    int32_t rc = pio_check(e, 1, (__force uintptr_t)buf, requested, pos, &entry);
    uint32_t count = USER_RW_LEN(requested);

    if (rc != 0 || count == 0) {
        return rc;
    }
    if (entry->type == FD_TYPE_FILE && !entry->ops) {
        return file_write(entry, buf, count, &at, count);
    }
    if (!entry->ops || !entry->ops->write || !entry->ofd) {
        return -EBADF;
    }
    pio_view(entry, at, &view, &desc);
    return entry->ops->write(&view, buf, count);
}

/** The descriptor is looked up once, and the call holds the big lock
 * unless its mode says its reads run without it (FD_MODE_READ_FREE,
 * `rw_call_lock`). */
long ksys_pread64(int fd, char __user *buf, size_t requested, long long pos)
{
    fd_entry_t *e = current_proc ? fd_get(current_proc->fds, fd) : NULL;
    int big = rw_call_lock(e, FD_MODE_READ_FREE);
    long rc = pread_at(e, buf, requested, pos);

    file_ops_unlock(big);
    return rc;
}

/** `pwrite_at` on the descriptor looked up once, under the big lock unless
 * its mode says its writes run without it (FD_MODE_WRITE_FREE,
 * `rw_call_lock`). */
long ksys_pwrite64(int fd, const char __user *buf, size_t requested, long long pos)
{
    fd_entry_t *e = current_proc ? fd_get(current_proc->fds, fd) : NULL;
    int big = rw_call_lock(e, FD_MODE_WRITE_FREE);
    long rc = pwrite_at(e, buf, requested, pos);

    file_ops_unlock(big);
    return rc;
}

/**
 * pread64(2) as a 64-bit architecture has it: the descriptor, the buffer,
 * the count and the offset, one register each. i386's, which splits the
 * offset, is in arch/i686/sys_ia32.c.
 */
long sys_pread64(isr_params_t *params)
{
    return ksys_pread64((int)SC_ARG(params, 0), (char __user *)SC_ARG(params, 1), SC_ARG(params, 2),
                        SC_SIGNED(params, 3));
}

/** pwrite64(2) as a 64-bit architecture has it, pread64's arguments. */
long sys_pwrite64(isr_params_t *params)
{
    return ksys_pwrite64((int)SC_ARG(params, 0), (const char __user *)SC_ARG(params, 1),
                         SC_ARG(params, 2), SC_SIGNED(params, 3));
}

/** The position preadv(2) and pwritev(2) take as two registers, low first:
 * the high one's bits lie above a long's, so on a 64-bit architecture the
 * low register is the whole position, as Linux's pos_from_hilo has it. */
static long long pos_from_hilo(unsigned long high, unsigned long low)
{
    return (long long)((((uint64_t)high << (sizeof(long) * 4)) << (sizeof(long) * 4)) | low);
}

/**
 * Move the first `wanted` bytes of the checked vector at `iov_user` from or,
 * with `write`, to `fd` at `pos`, element by element through `ksys_pread64`
 * or `ksys_pwrite64`, each at the position the ones before it reached. A
 * short transfer ends the call, and an error after bytes moved returns
 * those bytes. Returns the bytes moved or the first element's error.
 */
static long iov_at_move(int fd, uintptr_t iov_user, int iovcnt, long long pos, int write,
                        uint32_t wanted)
{
    struct kernel_iovec iov;
    long total = 0;
    int i;

    for (i = 0; i < iovcnt && (uint32_t)total < wanted; i++) {
        uint32_t len;
        long r;

        if (iov_fetch(&iov, iov_user, (uint32_t)i) != 0) {
            return total > 0 ? total : -EFAULT;
        }
        if (iov.iov_len == 0) {
            continue;
        }
        len = iov.iov_len < wanted - (uint32_t)total ? (uint32_t)iov.iov_len
                                                     : wanted - (uint32_t)total;
        r = write ? ksys_pwrite64(fd, (const char __user *)iov.iov_base, len, pos)
                  : ksys_pread64(fd, (char __user *)iov.iov_base, len, pos);
        if (r < 0) {
            return total > 0 ? total : r;
        }
        total += r;
        pos += r;
        if ((uint32_t)r < iov.iov_len) {
            break;
        }
    }
    return total;
}

/**
 * The elements of the vector at `iov_user` read from or, with `write`, written
 * to `fd` at the position `pos`, which the descriptor's own offset does not
 * follow. The position and the descriptor are checked first (`pio_entry`), as
 * Linux's do_preadv and vfs_readv, or do_pwritev and vfs_writev, test them
 * before the vector, a directory's EISDIR left to its read. A file of a
 * mount takes the vector whole, as Linux's vfs_iter_read and
 * vfs_iter_write take an iov_iter, with one lock across it and a bad buffer
 * ending the transfer with the bytes before it: a write through
 * iov_file_write, which an O_APPEND description takes before the vector is
 * imported, and cuts to the whole 2 MiB chunks a sealed tmpfs file lets in,
 * and a read through iov_file_read. Other transfers check the vector whole
 * (`iov_import`), so a bad element is refused before a byte moves, its
 * total cut at MAX_RW_COUNT, and then move it (`iov_at_move`). A vector of
 * no bytes has the descriptor checked again by the transfer, but for a
 * directory's, which is 0, as Linux's vfs_readv returns before the read.
 * Returns the bytes moved or a negative errno: -EINVAL for a negative
 * position, a count past IOV_MAX_LOCAL or below 0, a negative length, or a
 * vector whose end would pass 2^63-1; -EBADF or -ESPIPE for a descriptor
 * `pio_entry` refuses; -EFAULT for a vector or a first buffer the caller
 * cannot reach; -EPERM, with nothing written, for a write to a file sealed
 * against growth of which no whole 2 MiB chunk before the end fits, as
 * Linux takes the vector as one stream; or the first element's error.
 */
static long iov_at(int fd, uintptr_t iov_user, int iovcnt, long long pos, int write)
{
    fd_entry_t *entry = NULL, *held;
    uint32_t wanted;
    long rc = pio_entry(fd, write, 0, 0, pos, &entry);
    int dir = rc == -EISDIR, mounted;

    if (rc != 0 && !dir) {
        return rc;
    }
    if (iovcnt < 0 || iovcnt > IOV_MAX_LOCAL) {
        return -EINVAL;
    }
    held = entry;
    entry = write ? entry : NULL;
    if (iov_append_file(entry)) {
        return iov_file_write(entry, iov_user, iovcnt, pos, 1);
    }
    mounted = !dir && iov_mount_file(held);
    rc = iov_import(NULL, iov_user, iovcnt, mounted ? -1 : !write);
    if (rc < 0) {
        return rc;
    }
    wanted = (uint32_t)rc;
    if (wanted == 0) {
        if (dir) {
            return 0;
        }
        return write ? ksys_pwrite64(fd, NULL, 0, pos) : ksys_pread64(fd, NULL, 0, pos);
    }
    if (rw_end_past(held, (uint64_t)pos, (uint64_t)wanted)) {
        return -EINVAL;
    }
    if (mounted) {
        return write ? iov_file_write(held, iov_user, iovcnt, pos, 0)
                     : iov_file_read(held, iov_user, iovcnt, pos);
    }
    return iov_at_move(fd, iov_user, iovcnt, pos, write, wanted);
}

/** preadv(2): the descriptor, the vector, its count and the position as two
 * registers, low first. */
long sys_preadv(isr_params_t *params)
{
    return iov_at((int)SC_ARG(params, 0), SC_ARG(params, 1), (int)SC_ARG(params, 2),
                  pos_from_hilo(SC_ARG(params, 4), SC_ARG(params, 3)), 0);
}

/** pwritev(2): preadv's arguments, the vector written from. */
long sys_pwritev(isr_params_t *params)
{
    return iov_at((int)SC_ARG(params, 0), SC_ARG(params, 1), (int)SC_ARG(params, 2),
                  pos_from_hilo(SC_ARG(params, 4), SC_ARG(params, 3)), 1);
}

/** Bring in the buffers of an imported vector before taking file locks, for
 * writing when `into`, as a read fills them, or for reading, as a write
 * takes them. At the first bad data page keep only the prefix before it,
 * including a partial element, and clear the lengths after it, so that the
 * transfer ends there with the bytes before it, as Linux's copy to or from
 * an iov_iter stops at the first fault. Return the accessible byte count.
 * Faults may sleep and take another file's lock, so this step precedes the
 * file's locks. */
static uint32_t iov_prefix(struct kernel_iovec *iov, int count, int into)
{
    uint32_t total = 0;
    int i;

    for (i = 0; i < count; i++) {
        uintptr_t addr = iov[i].iov_base;
        uint32_t len = (uint32_t)iov[i].iov_len, have = 0;

        if (!len || user_buffer_ok(addr, len, into)) {
            total += len;
            continue;
        }
        while (have < len) {
            uint32_t piece = PAGE_SIZE - ((addr + have) & (PAGE_SIZE - 1));

            if (piece > len - have) {
                piece = len - have;
            }
            if (!user_buffer_ok(addr + have, piece, into)) {
                break;
            }
            have += piece;
        }
        iov[i].iov_len = have;
        total += have;
        while (++i < count) {
            iov[i].iov_len = 0;
        }
        break;
    }
    return total;
}

/** Import a vector (`iov_import`), its total in `*wanted`, and retain its
 * prefix accessible for writing, when `into`, or for reading before
 * locking (`iov_prefix`). Return 0, an import error, or EFAULT when no
 * requested byte is accessible. */
static long iov_copy_in(struct kernel_iovec *iov, uintptr_t addr, int count, uint32_t *wanted,
                        int into)
{
    long rc = iov_import(iov, addr, count, -1);

    if (rc < 0) {
        return rc;
    }
    *wanted = (uint32_t)rc;
    return *wanted && iov_prefix(iov, count, into) == 0 ? -EFAULT : 0;
}

/** Cut the imported vector `iov` of `count` elements to its first `room`
 * bytes, clearing the lengths after them. */
static void iov_trim(struct kernel_iovec *iov, int count, uint32_t room)
{
    int i;

    for (i = 0; i < count; i++) {
        if (iov[i].iov_len > room) {
            iov[i].iov_len = room;
        }
        room -= (uint32_t)iov[i].iov_len;
    }
}

/** Write the imported vector with the description and file locks in
 * `state` held. An explicit `pos` advances only within this call; -1
 * advances the shared offset. Return the bytes written before a short
 * write or error, or the first error when no byte was written. */
static long iov_file_elements(fd_entry_t *entry, const struct kernel_iovec *iov, int count,
                              long long pos, struct file_write_state *state)
{
    uint64_t at = (uint64_t)pos;
    long total = 0, rc = 0;
    int i;

    for (i = 0; i < count; i++) {
        if (!iov[i].iov_len) {
            continue;
        }
        rc = file_write_locked(entry, (const char __user *)iov[i].iov_base,
                               (uint32_t)iov[i].iov_len, pos == -1 ? NULL : &at, state);
        if (rc < 0) {
            break;
        }
        total += rc;
        at += (uint64_t)rc;
        if ((unsigned long)rc < iov[i].iov_len) {
            break;
        }
    }
    return total > 0 ? total : rc;
}

/**
 * The refusals of `iov_file_write` under the description's lock of `entry`:
 * -EINVAL when, at the shared offset (`pos` -1), the end of `wanted` bytes
 * passes 2^63-1 (`rw_offset_end`), which a sharer's seek may have moved
 * since the caller's check, then -EBUSY for a file that backs a loop
 * device, or 0.
 */
static long iov_file_refused(fd_entry_t *entry, long long pos, uint32_t wanted)
{
    if (pos == -1 && rw_offset_end(entry, wanted) != 0) {
        return -EINVAL;
    }
    return loop_backs(entry->ofd->vmnt, entry->ofd->vnode) ? -EBUSY : 0;
}

/**
 * Write the vector of `count` elements at `addr` to the regular file of the
 * descriptor `entry`, which the caller holds, holding its description and file
 * locks across the elements (`file_write_locked`), or the mount's namespace
 * lock on a type without per-file locks. The vector is copied and range-checked
 * first, and its accessible prefix is brought in before the locks, since a
 * fault may need another file's lock. The descriptor is checked after those
 * copies, which may sleep. The explicit position `pos` is ignored when `append`
 * requests end placement; otherwise it is used even on an O_APPEND description.
 * -1 alone uses and advances the shared offset. The shared O_APPEND flag is
 * never changed. A vector that would grow a sealed tmpfs file is cut to the
 * whole 2 MiB chunks before its end (`iov_seal_grow`). Returns the bytes
 * written, including those before a bad data page, or an error when no byte was
 * written: -EINVAL for a count below 0 or past IOV_MAX_LOCAL, a negative
 * length, or a vector whose end at `pos`, or at the shared offset under the
 * description's lock (`iov_file_refused`), would pass 2^63-1, -EFAULT for a
 * vector or a buffer the caller cannot reach, -EBADF for a descriptor that is
 * not a file of a mount open for writing, -EBUSY for a file that backs a loop
 * device, or the write's error.
 */
static long iov_file_write(fd_entry_t *entry, uintptr_t addr, int count, long long pos, int append)
{
    struct kernel_iovec *iov;
    struct open_file *ofd;
    struct file_write_state state;
    uint32_t wanted = 0, room = 0;
    long rc = 0;

    if (count < 0 || count > IOV_MAX_LOCAL) {
        return -EINVAL;
    }
    iov = kmalloc((size_t)(count > 0 ? count : 1) * sizeof(*iov));
    if (!iov) {
        return -ENOMEM;
    }
    rc = iov_copy_in(iov, addr, count, &wanted, 0);
    if (rc != 0) {
        goto out;
    }
    ofd = entry->ofd;
    if (entry->type != FD_TYPE_FILE || entry->ops || !ofd || !ofd->vmnt ||
        !(entry->flags & FD_FLAG_WRITE)) {
        rc = -EBADF;
        goto out;
    }
    if (wanted == 0) {
        goto out;
    }
    if (pos >= 0 && (uint64_t)wanted > 0x7FFFFFFFFFFFFFFFull - (uint64_t)pos) {
        rc = -EINVAL; /* rw_verify_area's check, which O_APPEND does not skip */
        goto out;
    }
    fd_ofd_lock(ofd);
    state.own = file_lock(ofd, &state.file);
    if (!state.own) {
        vfs_node_lock(&state.file);
    }
    state.append = append;
    state.moved = 0;
    state.big = 0;
    rc = iov_file_refused(entry, pos, wanted);
    if (rc == 0) {
        rc = iov_seal_grow(ofd, pos == -1 ? ofd->offset : (uint64_t)pos, wanted, append, &room);
    }
    if (rc == 0 && room < wanted) {
        iov_trim(iov, count, room);
    }
    if (rc == 0) {
        rc = iov_file_elements(entry, iov, count, pos, &state);
    }
    if (!state.own) {
        vfs_node_unlock(&state.file);
    }
    file_unlock(&state.file, state.own);
    fd_ofd_unlock(ofd);
    if (rc > 0 && inotify_watches) {
        if (!state.big) {
            state.big = big_take();
        }
        inotify_desc(ofd, IN_MODIFY);
    }
    big_give(state.big);
    rc = write_synced(entry, rc);
out:
    kfree(iov);
    return rc;
}

/** Read the elements of the imported vector `iov` of `count` from the file
 * of `ofd`, whose description's lock the caller holds, at the description's
 * offset, which moves past the bytes read, for `pos` -1, or at `pos`, each
 * element where the one before it ended (`file_read_locked`), which sets
 * `*big` when it takes the big lock for the caller to give back. Return the
 * bytes read, stopping at the first element not filled, or when none was
 * read 0 at the end of the file or the read's error. */
static long iov_file_elements_read(struct open_file *ofd, const struct kernel_iovec *iov, int count,
                                   long long pos, int *big)
{
    uint64_t at = (uint64_t)pos;
    long total = 0;
    int32_t r = 0;
    int i;

    for (i = 0; i < count; i++) {
        uint32_t len = (uint32_t)iov[i].iov_len;

        if (len == 0) {
            continue;
        }
        r = file_read_locked(ofd, (char __user *)iov[i].iov_base, len, pos == -1 ? ofd->offset : at,
                             big);
        if (r <= 0) {
            break;
        }
        if (pos == -1) {
            ofd->offset += (uint64_t)r;
        }
        at += (uint64_t)r;
        total += r;
        if ((uint32_t)r < len) {
            break;
        }
    }
    return total > 0 ? total : r;
}

/**
 * Read the vector of `count` elements at `addr` from the file of a mount
 * that the descriptor `entry`, held by the caller, is open on, as Linux's
 * vfs_iter_read reads an iov_iter whole: at the description's offset for
 * `pos` -1, which moves past the bytes read, or at `pos`, which leaves it.
 * The vector is copied and range-checked once (`iov_import`), and its
 * buffers are brought in for writing before the lock, keeping the bytes
 * before the first that cannot be written (`iov_prefix`), so that the read
 * ends there with the bytes before it, as Linux's copy to an iov_iter stops
 * at its first fault. The description's lock is held across the elements
 * and the check of the vector's end at the offset, as Linux holds
 * f_pos_lock, so that a sharer's seek cannot come between them. Each
 * element is read from where the one before it ended (`file_read_locked`),
 * which reads the pages mappings of the file hold from memory, so that an
 * element sees what an earlier one stored into a mapping of the file, as
 * Linux's filemap_read reads the shared folio. Returns the bytes read,
 * stopping at the first element not filled, or when none was read 0 at the
 * end of the file or a negative errno: -EINVAL for a count below 0 or past
 * IOV_MAX_LOCAL, a negative length or an end past 2^63-1, -EFAULT for a
 * vector the caller cannot read or, where the file has a byte to read, a
 * first buffer it cannot write (`file_read_unwritable`), -ENOMEM, -EBADF
 * for no file, or the type's error. The end check and the file's end come
 * before that -EFAULT, as Linux's rw_verify_area and filemap_read run
 * before any copy.
 * A read of a byte or more reports IN_ACCESS to inotify. May sleep.
 */
static long iov_file_read(fd_entry_t *entry, uintptr_t addr, int count, long long pos)
{
    struct kernel_iovec local[8], *iov = local;
    struct open_file *ofd = entry->ofd;
    uint32_t wanted = 0, room = 0;
    int big = 0;
    long rc;

    if (count < 0 || count > IOV_MAX_LOCAL) {
        return -EINVAL;
    }
    if (count > (int)(sizeof(local) / sizeof(local[0]))) {
        iov = kmalloc((size_t)count * sizeof(*iov));
        if (!iov) {
            return -ENOMEM;
        }
    }
    rc = iov_import(iov, addr, count, -1);
    if (rc >= 0) {
        wanted = (uint32_t)rc;
        room = wanted ? iov_prefix(iov, count, 1) : 0;
        rc = 0;
    }
    if (rc == 0 && wanted > 0 && (!ofd || !ofd->vmnt)) {
        rc = -EBADF;
    } else if (rc == 0 && wanted > 0 && pos >= 0 &&
               (uint64_t)wanted > 0x7FFFFFFFFFFFFFFFull - (uint64_t)pos) {
        rc = -EINVAL;
    } else if (rc == 0 && wanted > 0) {
        fd_ofd_lock(ofd);
        if (pos == -1 && rw_offset_end(entry, wanted) != 0) {
            rc = -EINVAL;
        } else if (room == 0) {
            rc = file_read_unwritable(ofd, pos == -1 ? ofd->offset : (uint64_t)pos);
        } else {
            rc = iov_file_elements_read(ofd, iov, count, pos, &big);
        }
        fd_ofd_unlock(ofd);
        if (rc > 0 && inotify_watches) {
            inotify_desc(ofd, IN_ACCESS);
        }
        if (big) {
            bkl_exit();
        }
    }
    if (iov != local) {
        kfree(iov);
    }
    return rc;
}

/* Linux 6.12's per-call read/write flags; NOWAIT and ATOMIC are unsupported. */
#define RWF_HIPRI     0x01u
#define RWF_DSYNC     0x02u
#define RWF_SYNC      0x04u
#define RWF_NOWAIT    0x08u
#define RWF_APPEND    0x10u
#define RWF_NOAPPEND  0x20u
#define RWF_ATOMIC    0x40u
#define RWF_SUPPORTED 0x7fu

/**
 * The descriptor checks of preadv2 and pwritev2, which Linux makes before
 * it imports the vector: -EINVAL for a position below -1, -EBADF for a
 * descriptor `fd` that is not open; at another position, -ESPIPE for one
 * that cannot seek; then -EBADF for the wrong direction. At a position
 * other than -1 these are `pio_entry`'s errors but
 * -EISDIR, which the read itself answers. 0 with the held entry in
 * `*entry`, NULL for a directory.
 */
static long rwv2_entry(int fd, long long pos, int write, fd_entry_t **entry)
{
    fd_entry_t *e;
    long rc;

    *entry = NULL;
    if (pos < -1) {
        return -EINVAL;
    }
    if (!current_proc) {
        return -ESRCH;
    }
    if (pos != -1) {
        rc = pio_entry(fd, write, 0, 0, pos, entry);
        if (rc == -EISDIR) {
            *entry = NULL;
            return 0;
        }
        return rc;
    }
    e = fd_get(current_proc->fds, fd);
    if (!e || (e->flags & FD_FLAG_PATH) || !(e->flags & (write ? FD_FLAG_WRITE : FD_FLAG_READ))) {
        return -EBADF;
    }
    *entry = e;
    return 0;
}

/**
 * Import `count` vectors from `addr` into `iov`, which holds that many
 * native elements, by `iov_import`, leaving faulting in the data pages to
 * the transfer. Returns the byte count or -EFAULT/-EINVAL. The result keeps
 * the caller's layout, packed back into 32-bit pairs for an i386 entry, for
 * iov_fetch to read through a user-copy window. The caller checks count
 * before allocating iov.
 */
static long rwv2_import(struct kernel_iovec *iov, uintptr_t addr, int count)
{
    long total = iov_import(iov, addr, count, -1);
    int i;

    if (total < 0) {
        return total;
    }
    if (in_compat_syscall()) {
        for (i = 0; i < count; i++) {
            uint32_t pair[2];

            pair[0] = (uint32_t)iov[i].iov_base;
            pair[1] = (uint32_t)iov[i].iov_len;
            k_memcpy((uint8_t *)iov + (size_t)i * sizeof(pair), pair, sizeof(pair));
        }
    }
    return total;
}

/** Transfer the copied vector at `addr`, opened to the user-copy checks,
 * using the held descriptor `entry` and the arguments of `params`:
 * APPEND ignores the position, NOAPPEND overrides O_APPEND for this call,
 * -1 uses the shared position, and a
 * positive write with SYNC or DSYNC flushes the
 * description the call holds. Returns the byte count or -errno. */
static long rwv2_transfer(isr_params_t *params, fd_entry_t *entry, uintptr_t addr, int count,
                          int write)
{
    int fd = (int)SC_ARG(params, 0);
    long long pos = pos_from_hilo(SC_ARG(params, 4), SC_ARG(params, 3));
    uint32_t flags = (uint32_t)SC_ARG(params, 5);
    long rc;

    if (write && (flags & (RWF_APPEND | RWF_NOAPPEND)) && entry && entry->type == FD_TYPE_FILE &&
        !entry->ops && entry->ofd && entry->ofd->vmnt) {
        rc = iov_file_write(entry, addr, count, pos, (flags & RWF_APPEND) != 0);
    } else if (pos == -1) {
        rc = write ? writev(fd, addr, count) : readv(fd, addr, count);
    } else {
        rc = iov_at(fd, addr, count, pos, write);
    }
    if (rc > 0 && write && entry && (flags & (RWF_DSYNC | RWF_SYNC))) {
        long synced = sync_entry(entry, (flags & RWF_SYNC) ? vfs_fsync : vfs_fdatasync);

        if (synced < 0) {
            return synced;
        }
    }
    return rc;
}

/**
 * preadv2 or pwritev2 in Linux's order: a position below -1 is -EINVAL;
 * a descriptor not open is -EBADF; at a position other than -1, one that
 * cannot seek is -ESPIPE; then the wrong direction is -EBADF. Import the
 * vector next, return zero for no bytes, then refuse unsupported flags
 * with -EOPNOTSUPP. APPEND with NOAPPEND is -EINVAL, after unknown flags
 * but before known flags the file does not support, and NOAPPEND on an
 * O_APPEND description of an append-only file is -EPERM after those, as
 * Linux 6.12's kiocb_set_rw_flags orders them. A data page is faulted in
 * only afterward. The vector is kept in the caller's layout and opened to
 * the shared readv/writev paths, so each reads the same copy across
 * sleeping I/O.
 * The other arguments and errors are preadv's and pwritev's.
 */
static long rwv2(isr_params_t *params, int write)
{
    long long pos = pos_from_hilo(SC_ARG(params, 4), SC_ARG(params, 3));
    uint32_t flags = (uint32_t)SC_ARG(params, 5);
    int fd = (int)SC_ARG(params, 0), count = (int)SC_ARG(params, 2);
    struct kernel_iovec local[8], *iov = local;
    fd_entry_t *entry = NULL;
    long rc = rwv2_entry(fd, pos, write, &entry);

    if (rc != 0) {
        return rc;
    }
    if (SC_ARG(params, 2) > IOV_MAX_LOCAL) {
        return -EINVAL;
    }
    if (count > (int)(sizeof(local) / sizeof(local[0]))) {
        iov = kmalloc((size_t)count * sizeof(*iov));
        if (!iov) {
            return -ENOMEM;
        }
    }
    rc = rwv2_import(iov, SC_ARG(params, 1), count);
    if (rc <= 0) {
        goto out;
    }
    if (pos == -1 ? rw_offset_end(entry, (size_t)rc) != 0
                  : rw_end_past(entry, (uint64_t)pos, (uint64_t)rc)) {
        rc = -EINVAL;
        goto out;
    }
    if (flags & ~RWF_SUPPORTED) {
        rc = -EOPNOTSUPP;
        goto out;
    }
    if ((flags & RWF_APPEND) && (flags & RWF_NOAPPEND)) {
        rc = -EINVAL;
        goto out;
    }
    if (flags & (RWF_NOWAIT | RWF_ATOMIC)) {
        rc = -EOPNOTSUPP;
        goto out;
    }
    if ((flags & RWF_NOAPPEND) && entry && entry->ofd && (entry->ofd->status_flags & OFD_APPEND) &&
        ofd_append_only(entry->ofd)) {
        rc = -EPERM;
        goto out;
    }
    user_window_open(iov, (uint32_t)((size_t)count * sizeof(*iov)));
    rc = rwv2_transfer(params, entry, (uintptr_t)iov, count, write);
    user_window_close();
out:
    if (iov != local) {
        kfree(iov);
    }
    return rc;
}

/** preadv2(2): preadv's arguments followed by per-call flags. */
long sys_preadv2(isr_params_t *params)
{
    return rwv2(params, 0);
}

/** pwritev2(2): pwritev's arguments followed by per-call flags. */
long sys_pwritev2(isr_params_t *params)
{
    return rwv2(params, 1);
}

/* fcntl64's lock commands: struct flock, with 32-bit offsets, for the first
 * three; struct flock64 for the 64-bit forms and the open file description
 * ones. */
#define F_GETLK      5
#define F_SETLK      6
#define F_SETLKW     7
#define F_GETLK64    12
#define F_SETLK64    13
#define F_SETLKW64   14
#define F_OFD_GETLK  36
#define F_OFD_SETLK  37
#define F_OFD_SETLKW 38

/**
 * The byte range a struct flock names, into `req`, as Linux reads it: from
 * the file's start, the description's offset or the file's end by `whence`,
 * backwards from `start` for a negative length, and to LOCK_EOF, the end
 * however far the file grows, for a length of 0. A range whose last byte is
 * the largest offset is that same range, as Linux has it, and is given as
 * LOCK_EOF too, so the two join and F_GETLK reports either with a length of
 * 0. The end of a file of a mounted type is its size as the file system
 * reports it now, and one of anything else, a pipe or a device, is 0.
 * Returns 0, -EINVAL for a `whence` of none of the three or a range before
 * byte 0, -EOVERFLOW for one past the largest offset, or the error the file
 * system gives for its size, EIO for a removed device among them.
 */
static int32_t flock_range(const fd_entry_t *entry, int whence, int64_t start, int64_t len,
                           struct lock_req *req)
{
    const int64_t max = 0x7FFFFFFFFFFFFFFFll;
    int64_t base = 0;
    struct vfs_stat st;
    int32_t rc;

    if (whence == 1) {
        base = (int64_t)entry->ofd->offset;
    } else if (whence == 2 && entry->ofd->vmnt) {
        rc = vfs_getattr(entry->ofd->vmnt, entry->ofd->vnode, &st);
        if (rc != 0) {
            return rc;
        }
        base = (int64_t)st.size;
    } else if (whence != 0 && whence != 2) {
        return -EINVAL;
    }
    if (start > 0 && base > max - start) {
        return -EOVERFLOW;
    }
    start += base;
    if (start < 0) {
        return -EINVAL;
    }
    if (len > 0 && len - 1 > max - start) {
        return -EOVERFLOW;
    }
    if (len < 0 && start + len < 0) {
        return -EINVAL;
    }
    req->start = (uint64_t)(len < 0 ? start + len : start);
    req->end = len > 0 ? (uint64_t)(start + (len - 1)) : len < 0 ? (uint64_t)(start - 1) : LOCK_EOF;
    if (req->end == (uint64_t)max) {
        req->end = LOCK_EOF;
    }
    return 0;
}

/**
 * The lock operation an fcntl64 lock command asks for: LOCK_GET, LOCK_SET or
 * LOCK_WAIT, with `*wide` set for the commands that take struct flock64 and
 * `*by_ofd` for the open file description ones.
 */
static int flock_cmd(int cmd, int *wide, int *by_ofd)
{
    *wide = cmd != F_GETLK && cmd != F_SETLK && cmd != F_SETLKW;
    *by_ofd = cmd == F_OFD_GETLK || cmd == F_OFD_SETLK || cmd == F_OFD_SETLKW;
    switch (cmd) {
    case F_GETLK:
    case F_GETLK64:
    case F_OFD_GETLK:
        return LOCK_GET;
    case F_SETLKW:
    case F_SETLKW64:
    case F_OFD_SETLKW:
        return LOCK_WAIT;
    default:
        return LOCK_SET;
    }
}

/* Linux's struct flock, whose offsets are longs, and struct flock64, whose
 * offsets are 64 bits: the two are one layout where a long has 64 bits. */
struct kernel_flock {
    int16_t type;
    int16_t whence;
    long start;
    long len;
    int32_t pid;
};

struct kernel_flock64 {
    int16_t type;
    int16_t whence;
    int64_t start;
    int64_t len;
    int32_t pid;
};

/* Whether an offset fits the long of a struct flock. */
#define FITS_LONG(v) ((uint64_t)(v) <= (uint64_t)(~0UL >> 1))

/* The fields of a struct flock or struct flock64. */
struct flock_args {
    int16_t type;
    int16_t whence;
    int64_t start;
    int64_t len;
    int32_t pid;
};

/**
 * Read the caller's lock description at `arg` into `fl`: struct flock64, 24
 * bytes, when `wide`, and otherwise struct flock, 16 bytes with 32-bit
 * offsets. Returns 0 or -EFAULT.
 */
static int32_t flock_read(uintptr_t arg, int wide, struct flock_args *fl)
{
    if (wide) {
        struct kernel_flock64 f;

        if (copy_from_user(&f, (const void __user *)arg, sizeof(f)) != 0) {
            return -EFAULT;
        }
        fl->type = f.type;
        fl->whence = f.whence;
        fl->start = f.start;
        fl->len = f.len;
        fl->pid = f.pid;
    } else {
        struct kernel_flock f;

        if (copy_from_user(&f, (const void __user *)arg, sizeof(f)) != 0) {
            return -EFAULT;
        }
        fl->type = f.type;
        fl->whence = f.whence;
        fl->start = f.start;
        fl->len = f.len;
        fl->pid = f.pid;
    }
    return 0;
}

/**
 * The pid `pid` of F_GETLK's answer, a kernel id, as the caller sees it in
 * its PID namespace (`pid_seen`), or `pid` itself when it is not positive,
 * -1 for a description's lock. `pid_seen` reads the process table, which
 * the big lock guards, so the big lock is taken here when the caller runs
 * without it, as Linux translates the pid under RCU (fs/locks.c,
 * locks_translate_pid). An owner reaped since the lock was found reads as
 * 0, as on Linux.
 */
static int32_t flock_pid_seen(int32_t pid)
{
    int take = !bkl_held();
    uint32_t seen;

    if (pid <= 0) {
        return pid;
    }
    if (take) {
        bkl_enter();
    }
    seen = pid_seen((uint32_t)pid);
    if (take) {
        bkl_exit();
    }
    return (int32_t)seen;
}

/**
 * Write F_GETLK's answer, the lock `req` found, over the caller's lock
 * description at `arg`, in the layout `flock_read` read: its type, its range
 * from the start of the file with a length of 0 for one to the end, and its
 * owner's pid as the caller sees it (`flock_pid_seen`); or F_UNLCK in
 * `l_type` alone when none conflicts. Returns 0, -EFAULT, or -EOVERFLOW for
 * a lock struct flock cannot describe.
 */
static int32_t flock_write(uintptr_t arg, int wide, const struct lock_req *req)
{
    int64_t start = (int64_t)req->start;
    int64_t len = req->end == LOCK_EOF ? 0 : (int64_t)(req->end - req->start + 1u);
    int32_t pid = flock_pid_seen(req->pid);
    int16_t type = (int16_t)req->type;

    if (req->type == F_UNLCK) {
        return copy_to_user((void __user *)arg, &type, sizeof(type)) != 0 ? -EFAULT : 0;
    }
    if (wide) {
        struct kernel_flock64 f;

        k_memset(&f, 0, sizeof(f));
        f.type = type;
        f.start = start;
        f.len = len;
        f.pid = pid;
        return copy_to_user((void __user *)arg, &f, sizeof(f)) != 0 ? -EFAULT : 0;
    } else {
        struct kernel_flock f;

        if (!FITS_LONG(req->start) || (req->end != LOCK_EOF && !FITS_LONG(req->end))) {
            return -EOVERFLOW;
        }
        k_memset(&f, 0, sizeof(f));
        f.type = type;
        f.start = (long)start;
        f.len = (long)len;
        f.pid = pid;
        return copy_to_user((void __user *)arg, &f, sizeof(f)) != 0 ? -EFAULT : 0;
    }
}

/**
 * Whether descriptor `entry` of the caller's table is not being closed,
 * read under the table's lock after a lock command set its lock, as Linux's
 * fcntl_setlk looks the descriptor up again under `file_lock`
 * (fs/locks.c:2468-2482): a close marks the descriptor closing under that
 * lock before it drops the table's locks (`close_fd`), so either this sees
 * the mark or the close sees the lock.
 */
static int fd_lock_still_open(const fd_entry_t *entry)
{
    uint32_t flags = fd_table_lock(current_proc->fds);
    int open = !(entry->flags & FD_FLAG_CLOSING);

    fd_table_unlock(current_proc->fds, flags);
    return open;
}

/**
 * The record-lock commands of fcntl64 on `entry`: `cmd` one of the nine,
 * `arg` the caller's struct flock, 16 bytes with 32-bit offsets for F_GETLK,
 * F_SETLK and F_SETLKW, or struct flock64, 24 bytes, for the rest. Returns
 * 0 or a negative errno.
 *
 * A POSIX lock belongs to the descriptor table and an open file description
 * lock (F_OFD_*, whose `l_pid` must be 0) to the description, as
 * lock.Readme.md describes. Setting a read lock needs a descriptor open for
 * reading and a write lock one open for writing (EBADF). F_GETLK reports the
 * first lock that conflicts, with its range from the start of the file, a
 * length of 0 for one to the end, and the pid of the process that took it,
 * -1 for a description's; or F_UNLCK in `l_type` alone when none does. The
 * 32-bit form answers EOVERFLOW for a lock it cannot describe.
 *
 * A thread that shares the descriptor table may close the descriptor while
 * this call waits for a lock or reads the caller's structure; the close
 * drops the table's locks on the file and only marks the descriptor closing
 * while the call has it. A POSIX lock gained after that is let go with the
 * table's others on the file and the call answers EBADF, as Linux's does,
 * so no lock outlives the close that should have dropped it.
 * The file's key is taken again for that, since a KRFFS file a write moved
 * during the wait has a new one. An OFD lock belongs to the description,
 * which the call still holds.
 *
 * The call runs without the big lock, as Linux's fcntl_setlk takes none:
 * the locks have their own (lock.c). It takes the big lock for a file keyed
 * by its node (`fd_locks_by_node`), for a SEEK_END range on a file whose
 * attributes need it, which `file_vfs_unlocked` does not admit, and for
 * F_GETLK's translation of the owner's pid into the caller's PID namespace
 * (`flock_pid_seen`).
 */
static int32_t fcntl_lock(fd_entry_t *entry, int cmd, uintptr_t arg)
{
    int wide, by_ofd, big;
    int op = flock_cmd(cmd, &wide, &by_ofd);
    struct flock_args fl;
    struct lock_req req;
    struct lock_key key;
    int32_t rc;

    if (!entry->ofd) {
        return -EBADF;
    }
    rc = flock_read(arg, wide, &fl);
    if (rc != 0) {
        return rc;
    }
    if (by_ofd && fl.pid != 0) {
        return -EINVAL;
    }
    if (op != LOCK_GET && ((fl.type == F_RDLCK && !(entry->flags & FD_FLAG_READ)) ||
                           (fl.type == F_WRLCK && !(entry->flags & FD_FLAG_WRITE)))) {
        return -EBADF;
    }
    big = fd_locks_by_node(entry) ||
          (fl.whence == 2 && entry->ofd->vmnt && !file_vfs_unlocked(entry));
    if (big) {
        bkl_enter();
    }
    rc = flock_range(entry, fl.whence, fl.start, fl.len, &req);
    if (rc == 0) {
        req.type = fl.type;
        req.pid = 0;
        fd_lock_key(entry, &key);
        rc = lock_record(&key, by_ofd ? entry->ofd : NULL, op, &req);
    }
    if (rc == 0 && op != LOCK_GET && req.type != F_UNLCK && !by_ofd && !fd_lock_still_open(entry)) {
        fd_lock_key(entry, &key);
        lock_close(&key);
        rc = -EBADF;
    }
    if (big) {
        bkl_exit();
    }
    if (rc != 0 || op != LOCK_GET) {
        return rc;
    }
    return flock_write(arg, wide, &req);
}

/* fcntl's seal commands, on a tmpfs file. */
#define F_ADD_SEALS 1033
#define F_GET_SEALS 1034

/**
 * F_ADD_SEALS and F_GET_SEALS on `entry`: the seals of a tmpfs file
 * (tmpfs.h), which a memfd made with MFD_ALLOW_SEALING starts without and
 * every other tmpfs file starts with F_SEAL_SEAL alone, which refuses more.
 * A descriptor of anything else, a file of memfd_secret among them, is
 * EINVAL; adding needs a descriptor open
 * for writing (EPERM), then `arg` must be seals (EINVAL) and the file not
 * sealed against sealing (EPERM), in Linux's order. F_SEAL_WRITE does not
 * wait for writable shared mappings to go, as Linux's does, and is not
 * refused with EBUSY while one exists. Returns the seals for F_GET_SEALS,
 * 0 for F_ADD_SEALS, or a negative errno.
 */
static int32_t fcntl_seals(const fd_entry_t *entry, int cmd, uint32_t arg)
{
    if (entry->type != FD_TYPE_FILE || !entry->ofd || !entry->ofd->vmnt ||
        entry->ofd->vmnt->type != &vfs_tmpfs_type || tmpfs_secret(entry->ofd->vnode)) {
        return -EINVAL;
    }
    if (cmd == F_GET_SEALS) {
        return (int32_t)tmpfs_seals(entry->ofd->vnode);
    }
    if (!(entry->flags & FD_FLAG_WRITE)) {
        return -EPERM;
    }
    return tmpfs_add_seals(entry->ofd->vnode, arg);
}

/* memfd_create's flags. */
#define MFD_CLOEXEC       0x01u
#define MFD_ALLOW_SEALING 0x02u
#define MFD_NOEXEC_SEAL   0x08u
#define MFD_EXEC          0x10u

/* splice's and tee's flags: SPLICE_F_NONBLOCK, the one this kernel acts
 * on, and all four, of which a move or a gift of pages is a copy here and
 * SPLICE_F_MORE a hint. */
#define SPLICE_F_NONBLOCK 0x02u
#define SPLICE_F_ALL      0x0Fu

/* The bytes a copy between descriptors moves through its kernel buffer at a
 * time. */
#define COPY_CHUNK 32768u

/* The largest file offset, Linux's loff_t's, and the largest i386's sendfile
 * with a 32-bit offset reaches, Linux's MAX_NON_LFS. */
#define COPY_POS_MAX   ((((uint64_t)0x7FFFFFFFu) << 32) | 0xFFFFFFFFu)
#define COPY_POS_MAX32 0x7FFFFFFFu

/** The caller's descriptor `fd` for a copy, or NULL when it is not open or
 * was opened with O_PATH. */
static fd_entry_t *copy_fd(int fd)
{
    fd_entry_t *e = fd_get(current_proc->fds, fd);

    return e && !(e->flags & FD_FLAG_PATH) ? e : NULL;
}

/** Whether the description of `e` has O_NONBLOCK. */
static int copy_nonblock(const fd_entry_t *e)
{
    return e->ofd && (e->ofd->status_flags & OFD_NONBLOCK);
}

/**
 * Whether `e` is open on a regular file of the KRFFS root or of a mounted
 * type, which the copies read and write at an offset: 0, -EISDIR for a
 * directory, or -EINVAL for anything else, a FIFO, a device and a /proc
 * file among them.
 */
static int32_t copy_regular(fd_entry_t *e)
{
    struct vfs_stat st;

    if (e->type == FD_TYPE_DIR) {
        return -EISDIR;
    }
    if (e->type != FD_TYPE_FILE || e->ops || !e->ofd || !e->ofd->vmnt ||
        vfs_getattr(e->ofd->vmnt, e->ofd->vnode, &st) != 0) {
        return -EINVAL;
    }
    if ((st.mode & S_IFMT) == S_IFDIR) {
        return -EISDIR;
    }
    return (st.mode & S_IFMT) == S_IFREG ? 0 : -EINVAL;
}

/** Read the offset at the user's `uoff` into `*pos`: a 64-bit loff_t, or
 * with `narrow` i386's 32-bit off_t. Returns 0, -EFAULT, or -EINVAL for an
 * offset below 0. */
static int32_t copy_off_get(uintptr_t uoff, int narrow, uint64_t *pos)
{
    if (narrow) {
        int32_t v;

        if (copy_from_user(&v, (const void __user *)uoff, sizeof(v)) != 0) {
            return -EFAULT;
        }
        if (v < 0) {
            return -EINVAL;
        }
        *pos = (uint64_t)v;
    } else {
        long long v;

        if (copy_from_user(&v, (const void __user *)uoff, sizeof(v)) != 0) {
            return -EFAULT;
        }
        if (v < 0) {
            return -EINVAL;
        }
        *pos = (uint64_t)v;
    }
    return 0;
}

/** Store `pos` at the user's `uoff`, as `copy_off_get` reads it: 0 or
 * -EFAULT. */
static int32_t copy_off_put(uintptr_t uoff, int narrow, uint64_t pos)
{
    if (narrow) {
        int32_t v = (int32_t)pos;

        return copy_to_user((void __user *)uoff, &v, sizeof(v)) != 0 ? -EFAULT : 0;
    } else {
        long long v = (long long)pos;

        return copy_to_user((void __user *)uoff, &v, sizeof(v)) != 0 ? -EFAULT : 0;
    }
}

/**
 * The kernel buffer of a copy, COPY_CHUNK bytes from the heap opened to the
 * user-copy checks (`user_window_open`), so that the read and write paths,
 * which take user buffers, take it; NULL when the heap has no room. The
 * copy gives it back with `copy_buf_put`.
 */
static char *copy_buf_get(void)
{
    char *buf = kmalloc(COPY_CHUNK);

    if (buf) {
        user_window_open(buf, COPY_CHUNK);
    }
    return buf;
}

/** Close the window on a copy's buffer and free it. */
static void copy_buf_put(char *buf)
{
    user_window_close();
    kfree(buf);
}

/**
 * Write `n` bytes of a copy's buffer `buf` to `out`, the caller's descriptor
 * `out_fd`: to a pipe without waiting, since the caller waited for its room,
 * in a page buffer for each page of the source the bytes come from, whose
 * first byte lies `src` bytes into its page (`pipe_splice_write`); to a
 * regular file at `*at` when `at` is not NULL, as pwrite64 writes; and
 * otherwise as write(2) writes, at the description's offset and by its
 * O_NONBLOCK. Returns the bytes written or a negative errno.
 */
static int32_t copy_put(fd_entry_t *out, int out_fd, const char *buf, uint32_t n,
                        const uint64_t *at, uint32_t src)
{
    if (pipe_is(out)) {
        return pipe_splice_write(out, (const char __force __user *)buf, n, 1, src);
    }
    if (at) {
        return file_write(out, (const char __force __user *)buf, n, at, n);
    }
    return write_fd(out_fd, (const char __force __user *)buf, n, 1);
}

/**
 * Copy up to `count` bytes of the regular file `in` from `*pos` to `out`,
 * the caller's descriptor `out_fd`, at `*out_at` when that is not NULL, a
 * chunk at a time through `buf`, a copy's buffer (`copy_buf_get`). Each
 * chunk is read at `*pos` as pread64 reads, and `*pos` and `*out_at` move
 * past what `out` took.
 *
 * A pipe's room is waited for before the first chunk unless `nonblock`, and
 * a chunk is no larger than the room, so a copy into a pipe returns what
 * fitted, as Linux's splice does, and never holds bytes it cannot write. A
 * chunk the room cuts is the copy's last, and one COPY_CHUNK cuts ends at a
 * page of the file, so that each page is one buffer of the pipe, as Linux's
 * splice links it once. The copy ends at the end of the file, at a short
 * write and, once a byte has moved, at a signal. Returns the bytes copied,
 * or the first error when none were.
 */
static int32_t copy_from_file(fd_entry_t *in, uint64_t *pos, fd_entry_t *out, int out_fd,
                              uint64_t *out_at, uint32_t count, int nonblock, char *buf)
{
    uint32_t done = 0;

    while (done < count) {
        uint32_t want = count - done;
        uint32_t src;
        int full = 0;
        int32_t got, put;

        if (want > COPY_CHUNK) {
            want = COPY_CHUNK;
        }
        src = (uint32_t)(*pos & (PAGE_SIZE - 1));
        if (pipe_is(out)) {
            int32_t room = pipe_splice_room(out, nonblock || done > 0, src);

            if (room < 0) {
                return done > 0 ? (int32_t)done : room;
            }
            if ((uint32_t)room < want) {
                want = (uint32_t)room;
                full = 1;
            }
        }
        if (pipe_is(out) && !full && want < count - done) {
            want -= (src + want) & (PAGE_SIZE - 1);
        }
        got = file_read(in, (char __force __user *)buf, want, pos, want);
        if (got <= 0) {
            return done > 0 ? (int32_t)done : got;
        }
        put = copy_put(out, out_fd, buf, (uint32_t)got, out_at, src);
        if (put == -EAGAIN && pipe_is(out) && !nonblock && done == 0) {
            continue; /* another writer took the room while the read slept */
        }
        if (put < 0) {
            return done > 0 ? (int32_t)done : put;
        }
        *pos += (uint64_t)put;
        if (out_at) {
            *out_at += (uint64_t)put;
        }
        done += (uint32_t)put;
        if (full || put < got || (uint32_t)got < want || signal_pending()) {
            break;
        }
    }
    return (int32_t)done;
}

/**
 * Copy up to `count` bytes from the pipe of the read end `in` to `out`,
 * the caller's descriptor `out_fd` and not a pipe, at `*out_at` when that is
 * not NULL, which moves past what was written, through `buf` as
 * `copy_from_file` does. Each chunk is copied out of the ring and held,
 * written, and only then taken from the ring (`pipe_splice_consume`), so a
 * short or failed write leaves the rest in the pipe, and no other reader
 * takes the held bytes while the write sleeps. The pipe's bytes are waited for
 * before the first chunk unless `nonblock`. Returns the bytes copied, 0 for
 * an empty pipe with no writer, or the first error when none were.
 */
static int32_t copy_from_pipe(fd_entry_t *in, fd_entry_t *out, int out_fd, uint64_t *out_at,
                              uint32_t count, int nonblock, char *buf)
{
    uint32_t done = 0;

    while (done < count) {
        uint32_t want = count - done;
        int32_t got, put;

        if (want > COPY_CHUNK) {
            want = COPY_CHUNK;
        }
        got = pipe_splice_peek(in, (uint8_t *)buf, want, nonblock || done > 0);
        if (got <= 0) {
            return done > 0 ? (int32_t)done : got;
        }
        put = copy_put(out, out_fd, buf, (uint32_t)got, out_at, 0);
        pipe_splice_consume(in, put > 0 ? (uint32_t)put : 0);
        if (put < 0) {
            return done > 0 ? (int32_t)done : put;
        }
        if (out_at) {
            *out_at += (uint64_t)put;
        }
        done += (uint32_t)put;
        if (put < got || signal_pending()) {
            break;
        }
    }
    return (int32_t)done;
}

/** Store the offset a copy left in `pos` for the descriptor `e`: at the
 * user's `uoff` when it gave one, and otherwise as the description's
 * offset, under its lock. Returns 0 or -EFAULT. */
static int32_t copy_pos_done(fd_entry_t *e, uintptr_t uoff, int narrow, uint64_t pos)
{
    if (uoff) {
        return copy_off_put(uoff, narrow, pos);
    }
    fd_ofd_lock(e->ofd);
    e->ofd->offset = pos;
    fd_ofd_unlock(e->ofd);
    return 0;
}

static long splice_file(const isr_params_t *params, fd_entry_t *in, fd_entry_t *out);

/**
 * splice(2): move up to the length in argument 4 between the descriptors in
 * arguments 0 and 2, at least one of them a pipe, with the offsets at the
 * user's addresses in arguments 1 and 3 and the flags in argument 5.
 *
 * Pipe to pipe moves the bytes ring to ring (`pipe_tee`). Pipe to anything
 * else writes the pipe's bytes as write(2) would and takes from the pipe
 * what was written (`copy_from_pipe`), into a regular file at the offset at
 * argument 3, which moves past them, or at and past the description's. A
 * regular file to a pipe reads what fits in the pipe from the offset at
 * argument 1, or from and past the description's (`copy_from_file`). A
 * pipe end is waited on unless SPLICE_F_NONBLOCK or its description's
 * O_NONBLOCK; the other flags change nothing.
 *
 * The call runs without the big lock, as read(2) and write(2) do, and
 * takes it for a move to or from anything but a pipe (`splice_file`): pipe
 * to pipe needs only the pipes' locks, and a holder of the big lock that
 * waits for the next write would spin only while no other processor wants
 * the lock (`bkl_spin_ok`), so GNU cat, which splices its input pipe into a
 * pipe of its own, would sleep for each of the writer's records.
 *
 * Returns the bytes moved, 0 for a length of 0 or an empty pipe with no
 * writer, or a negative errno, in Linux's order: -EINVAL for an unknown
 * flag, -EBADF for a descriptor not open, -ESPIPE for an offset given for a
 * pipe, -EBADF for an input not open for reading or an output not open for
 * writing, -EINVAL for one pipe at both ends, for no pipe, for a source
 * other than a pipe or a regular file, for an output with O_APPEND, and
 * for an offset below 0 or given for an output that cannot seek, -EFAULT,
 * or the pipe's and the file's errors.
 */
long sys_splice(isr_params_t *params)
{
    int fd_in = (int)SC_ARG(params, 0);
    uintptr_t uoff_in = SC_ARG(params, 1);
    int fd_out = (int)SC_ARG(params, 2);
    uintptr_t uoff_out = SC_ARG(params, 3);
    uint32_t len = USER_RW_LEN(SC_ARG(params, 4));
    uint32_t flags = (uint32_t)SC_ARG(params, 5);
    int nonblock = (flags & SPLICE_F_NONBLOCK) != 0;
    fd_entry_t *in, *out;
    long rc;

    if (len == 0) {
        return 0;
    }
    if (flags & ~SPLICE_F_ALL) {
        return -EINVAL;
    }
    if (!current_proc) {
        return -ESRCH;
    }
    in = copy_fd(fd_in);
    out = copy_fd(fd_out);
    if (!in || !out) {
        return -EBADF;
    }
    if ((pipe_is(in) && uoff_in) || (pipe_is(out) && uoff_out)) {
        return -ESPIPE;
    }
    if (!(in->flags & FD_FLAG_READ) || !(out->flags & FD_FLAG_WRITE)) {
        return -EBADF;
    }
    if (pipe_is(in) && pipe_is(out)) {
        return pipe_tee(in, out, len, nonblock || copy_nonblock(in) || copy_nonblock(out), 1);
    }
    bkl_enter();
    rc = splice_file(params, in, out);
    bkl_exit();
    return rc;
}

/**
 * The part of splice(2) in `params` whose ends are a pipe and something
 * else, the descriptors `in` and `out`, which `sys_splice` has looked up
 * and checked with the call's flags and length. Called under the big lock.
 * Returns what `sys_splice` does.
 */
static long splice_file(const isr_params_t *params, fd_entry_t *in, fd_entry_t *out)
{
    uintptr_t uoff_in = SC_ARG(params, 1);
    int fd_out = (int)SC_ARG(params, 2);
    uintptr_t uoff_out = SC_ARG(params, 3);
    uint32_t len = USER_RW_LEN(SC_ARG(params, 4));
    int nonblock = (SC_ARG(params, 5) & SPLICE_F_NONBLOCK) != 0;
    uint64_t pos = 0;
    int32_t rc;
    char *buf;

    if (pipe_is(in)) {
        if (out->ofd && (out->ofd->status_flags & OFD_APPEND)) {
            return -EINVAL;
        }
        if (uoff_out) {
            if (copy_regular(out) != 0) {
                return -EINVAL;
            }
            rc = copy_off_get(uoff_out, 0, &pos);
            if (rc != 0) {
                return rc;
            }
        }
        buf = copy_buf_get();
        if (!buf) {
            return -ENOMEM;
        }
        rc = copy_from_pipe(in, out, fd_out, uoff_out ? &pos : NULL, len,
                            nonblock || copy_nonblock(in), buf);
        copy_buf_put(buf);
        if (rc > 0 && uoff_out && copy_off_put(uoff_out, 0, pos) != 0) {
            return -EFAULT;
        }
        return rc;
    }
    if (!pipe_is(out) || copy_regular(in) != 0) {
        return -EINVAL;
    }
    if (uoff_in) {
        rc = copy_off_get(uoff_in, 0, &pos);
        if (rc != 0) {
            return rc;
        }
    } else {
        pos = in->ofd->offset;
    }
    buf = copy_buf_get();
    if (!buf) {
        return -ENOMEM;
    }
    rc = copy_from_file(in, &pos, out, fd_out, NULL, len, nonblock || copy_nonblock(out), buf);
    copy_buf_put(buf);
    if (rc > 0 && copy_pos_done(in, uoff_in, 0, pos) != 0) {
        return -EFAULT;
    }
    return rc;
}

/**
 * tee(2): copy up to the length in argument 2 from the pipe of the read end
 * in argument 0 to that of the write end in argument 1, leaving the bytes in
 * the first (`pipe_tee`), with the flags in argument 3. Waits as splice
 * does. Returns the bytes copied, 0 for a length of 0 or an empty pipe with
 * no writer, or a negative errno: -EINVAL for an unknown flag, -EBADF for a
 * descriptor not open for its direction, -EINVAL for a descriptor that is no
 * pipe or one pipe at both ends, or the pipes' errors. It runs without the
 * big lock, as a splice between pipes does.
 */
long sys_tee(isr_params_t *params)
{
    uint32_t len = USER_RW_LEN(SC_ARG(params, 2));
    uint32_t flags = (uint32_t)SC_ARG(params, 3);
    fd_entry_t *in, *out;

    if (flags & ~SPLICE_F_ALL) {
        return -EINVAL;
    }
    if (len == 0) {
        return 0;
    }
    if (!current_proc) {
        return -ESRCH;
    }
    in = copy_fd((int)SC_ARG(params, 0));
    if (!in || !(in->flags & FD_FLAG_READ)) {
        return -EBADF;
    }
    out = copy_fd((int)SC_ARG(params, 1));
    if (!out || !(out->flags & FD_FLAG_WRITE)) {
        return -EBADF;
    }
    if (!pipe_is(in) || !pipe_is(out)) {
        return -EINVAL;
    }
    return pipe_tee(in, out, len,
                    (flags & SPLICE_F_NONBLOCK) || copy_nonblock(in) || copy_nonblock(out), 0);
}

/* The bytes vmsplice moves from a pipe through the kernel stack at a time. */
#define VMSPLICE_CHUNK 512u

/**
 * Copy the pipe of the read end `e` into the `iovcnt` buffers of the
 * caller's vector at `uiov`, in order, for vmsplice: the first buffer waits
 * for bytes unless `nonblock`, as a read does, and the rest take only what
 * the pipe holds, as Linux's vmsplice_to_user stops at an empty pipe once it
 * has copied. Bytes leave the pipe only once copied out, and no more than
 * `wanted`, the vector's total cut at MAX_RW_COUNT. Returns the bytes
 * copied, 0 for an empty pipe with no writer, or the first error.
 */
static long vmsplice_read(fd_entry_t *e, uintptr_t uiov, int iovcnt, uint32_t wanted, int nonblock)
{
    uint8_t chunk[VMSPLICE_CHUNK];
    struct kernel_iovec iov;
    long total = 0;
    int i;

    for (i = 0; i < iovcnt && (uint32_t)total < wanted; i++) {
        uintptr_t done = 0;

        if (iov_fetch(&iov, uiov, (uint32_t)i) != 0) {
            return total > 0 ? total : -EFAULT;
        }
        if (iov.iov_len > wanted - (uint32_t)total) {
            iov.iov_len = wanted - (uint32_t)total;
        }
        while (done < iov.iov_len) {
            uint32_t want = iov.iov_len - done > VMSPLICE_CHUNK ? VMSPLICE_CHUNK
                                                                : (uint32_t)(iov.iov_len - done);
            int32_t got = pipe_splice_peek(e, chunk, want, nonblock || total > 0);

            if (got <= 0) {
                return total > 0 ? total : got;
            }
            if (copy_to_user((void __user *)(iov.iov_base + done), chunk, (uint32_t)got) != 0) {
                pipe_splice_consume(e, 0);
                return total > 0 ? total : -EFAULT;
            }
            pipe_splice_consume(e, (uint32_t)got);
            done += (uint32_t)got;
            total += got;
        }
    }
    return total;
}

/**
 * Copy the `iovcnt` buffers of the caller's vector at `uiov` into the pipe
 * of the write end `e`, in order, for vmsplice (`pipe_vmsplice_write`),
 * waiting for room before the first byte unless `nonblock` and never after
 * it, as Linux's `vmsplice_to_pipe` waits once and takes what fits. A short
 * write ends the call, and so do `wanted` bytes, the vector's total cut at
 * MAX_RW_COUNT. Returns the bytes copied, or the first error when none were.
 */
static long vmsplice_write(fd_entry_t *e, uintptr_t uiov, int iovcnt, uint32_t wanted, int nonblock)
{
    struct kernel_iovec iov;
    long total = 0;
    int i;

    for (i = 0; i < iovcnt && (uint32_t)total < wanted; i++) {
        int32_t r;

        if (iov_fetch(&iov, uiov, (uint32_t)i) != 0) {
            return total > 0 ? total : -EFAULT;
        }
        if (iov.iov_len > wanted - (uint32_t)total) {
            iov.iov_len = wanted - (uint32_t)total;
        }
        if (iov.iov_len == 0) {
            continue;
        }
        r = pipe_vmsplice_write(e, (const char __user *)iov.iov_base, (uint32_t)iov.iov_len,
                                nonblock || total > 0);
        if (r < 0) {
            return total > 0 ? total : r;
        }
        total += r;
        if ((uint32_t)r < iov.iov_len) {
            break;
        }
    }
    return total;
}

/**
 * vmsplice(2): the descriptor of a pipe, a vector of the caller's buffers,
 * its count and the flags. Into a write end it copies the buffers into the
 * pipe, and from a read end the pipe into the buffers; a descriptor open
 * both ways writes, as Linux's does. Linux gives the pipe the caller's pages
 * where it can; this kernel copies the bytes, which is the same for a caller
 * that does not change a buffer after a SPLICE_F_GIFT, and refuses a source
 * page that Linux's get_user_pages refuses, such as `[vvar]` or an untouched
 * page of a mapping without PROT_READ, with -EFAULT. The import brings no
 * source page in, as Linux's import_iovec brings none, so that the pipe
 * judges each page before its own fault makes it present (`pipe_put`).
 * The pipe end waits unless SPLICE_F_NONBLOCK, whatever its description's
 * O_NONBLOCK, as Linux's vmsplice passes the caller's flags alone, a write
 * end only before its first byte, so that the call returns what fitted in
 * the pipe, as Linux's does. Returns the bytes
 * moved or a negative errno, in Linux's order: -EINVAL for an unknown flag,
 * -EBADF for a descriptor not open, -EINVAL for a count past IOV_MAX_LOCAL
 * or a buffer of a negative length (`iov_import`, which cuts the total at
 * MAX_RW_COUNT), -EFAULT for a vector or a buffer the caller cannot reach,
 * 0 for buffers of no bytes, -EBADF for a descriptor that is no pipe, or
 * the pipe's error.
 */
long sys_vmsplice(isr_params_t *params)
{
    uintptr_t uiov = SC_ARG(params, 1);
    unsigned long cnt = SC_ARG(params, 2);
    uint32_t flags = (uint32_t)SC_ARG(params, 3);
    fd_entry_t *e;
    int write, nonblock;
    long wanted;

    if (flags & ~SPLICE_F_ALL) {
        return -EINVAL;
    }
    if (!current_proc) {
        return -ESRCH;
    }
    e = copy_fd((int)SC_ARG(params, 0));
    if (!e || !(e->flags & (FD_FLAG_READ | FD_FLAG_WRITE))) {
        return -EBADF;
    }
    write = (e->flags & FD_FLAG_WRITE) != 0;
    if (cnt > IOV_MAX_LOCAL) {
        return -EINVAL;
    }
    wanted = iov_import(NULL, uiov, (int)cnt, write ? -1 : 1);
    if (wanted <= 0) {
        return wanted;
    }
    if (!pipe_is(e)) {
        return -EBADF;
    }
    nonblock = (flags & SPLICE_F_NONBLOCK) != 0;
    return write ? vmsplice_write(e, uiov, (int)cnt, (uint32_t)wanted, nonblock)
                 : vmsplice_read(e, uiov, (int)cnt, (uint32_t)wanted, nonblock);
}

/**
 * The body of sendfile(2): copy up to `count` bytes of the regular file open
 * on `in_fd` to `out_fd` (`copy_from_file`), from the offset at the user's
 * `uoff`, which moves past what was sent while the description's stays, or
 * from and past the description's offset when `uoff` is 0. With `narrow`
 * the offset is i386's 32-bit one, and the copy stops at the largest it
 * holds.
 *
 * A pipe takes what fits in it, waited for unless either description has
 * O_NONBLOCK; any other output is written as write(2) writes it, a socket
 * among them. Returns the bytes sent, or a negative errno in Linux's order:
 * -EBADF for an input not open for reading, -ESPIPE for an offset given for
 * a pipe, -EINVAL for an input that is not a regular file or an offset below
 * 0, -EFAULT, -EBADF for an output not open for writing, -EINVAL for one
 * with O_APPEND, -EOVERFLOW for a 32-bit offset at its limit, or the error
 * of the first read or write.
 */
static long do_sendfile(int out_fd, int in_fd, uintptr_t uoff, uint32_t count, int narrow)
{
    uint64_t limit = narrow ? COPY_POS_MAX32 : COPY_POS_MAX;
    fd_entry_t *in, *out;
    uint64_t pos;
    int32_t rc;
    char *buf;

    if (!current_proc) {
        return -ESRCH;
    }
    in = copy_fd(in_fd);
    if (!in || !(in->flags & FD_FLAG_READ)) {
        return -EBADF;
    }
    if (uoff && pipe_is(in)) {
        return -ESPIPE;
    }
    if (copy_regular(in) != 0) {
        return -EINVAL;
    }
    if (uoff) {
        rc = copy_off_get(uoff, narrow, &pos);
        if (rc != 0) {
            return rc;
        }
    } else {
        pos = in->ofd->offset;
    }
    out = copy_fd(out_fd);
    if (!out || !(out->flags & FD_FLAG_WRITE)) {
        return -EBADF;
    }
    if (!pipe_is(out) && out->ofd && (out->ofd->status_flags & OFD_APPEND)) {
        return -EINVAL;
    }
    if (pos + count > limit) {
        if (pos >= limit) {
            return -EOVERFLOW;
        }
        count = (uint32_t)(limit - pos);
    }
    if (count == 0) {
        return 0;
    }
    buf = copy_buf_get();
    if (!buf) {
        return -ENOMEM;
    }
    rc = copy_from_file(in, &pos, out, out_fd, NULL, count,
                        copy_nonblock(in) || (pipe_is(out) && copy_nonblock(out)), buf);
    copy_buf_put(buf);
    if (rc > 0 && copy_pos_done(in, uoff, narrow, pos) != 0) {
        return -EFAULT;
    }
    return rc;
}

/** sendfile(2) with a 64-bit offset: x86-64's sendfile and i386's
 * sendfile64, with the output descriptor, the input descriptor, the offset's
 * address and the count (`do_sendfile`). */
long sys_sendfile(isr_params_t *params)
{
    return do_sendfile((int)SC_ARG(params, 0), (int)SC_ARG(params, 1), SC_ARG(params, 2),
                       USER_RW_LEN(SC_ARG(params, 3)), 0);
}

/** i386's sendfile, whose offset is a 32-bit off_t (`do_sendfile`). */
long sys_sendfile32(isr_params_t *params)
{
    return do_sendfile((int)SC_ARG(params, 0), (int)SC_ARG(params, 1), SC_ARG(params, 2),
                       USER_RW_LEN(SC_ARG(params, 3)), 1);
}

/**
 * Whether the descriptors `a` and `b`, each open on a file of a mounted
 * type, reach one file: the key their locks are found by (`fd_lock_key`),
 * which names a file by the mount whose file system it is on, so that two
 * bind views of one file, whose mounts differ, are one file.
 */
static int copy_same_file(const fd_entry_t *a, const fd_entry_t *b)
{
    struct lock_key ka, kb;

    fd_lock_key(a, &ka);
    fd_lock_key(b, &kb);
    return ka.mnt == kb.mnt && ka.ino == kb.ino;
}

/**
 * copy_file_range(2): copy up to the length in argument 4 from the regular
 * file open on the descriptor in argument 0 to the one in argument 2, with
 * the offsets at the user's addresses in arguments 1 and 3, each moving
 * past what was copied, or the descriptions' offsets where an address is 0,
 * and the flags in argument 5, which must be 0. There is no file system
 * here that shares extents, so every copy is the generic one, through the
 * read and write paths (`copy_from_file`), between any two mounts, as Linux
 * falls back to for a file system without its own. The length is cut at
 * the end of the input.
 *
 * Returns the bytes copied, 0 at or past the end of the input, or a negative
 * errno in Linux's order: -EINVAL for a flag, -EBADF for a descriptor not
 * open, -EISDIR for a directory, -EINVAL for anything else not a regular
 * file, -EBADF for an input not open for reading or an output not open for
 * writing or with O_APPEND, -EFAULT, -EINVAL for an offset below 0,
 * -EOVERFLOW for a range past the largest offset, -EINVAL for ranges of one
 * file that overlap, or the error of the first read or write.
 */
long sys_copy_file_range(isr_params_t *params)
{
    uintptr_t uoff_in = SC_ARG(params, 1);
    int fd_out = (int)SC_ARG(params, 2);
    uintptr_t uoff_out = SC_ARG(params, 3);
    uint32_t len = USER_RW_LEN(SC_ARG(params, 4));
    struct vfs_stat st;
    fd_entry_t *in, *out;
    uint64_t pos_in, pos_out;
    int32_t rc;
    char *buf;

    if (SC_ARG(params, 5) != 0) {
        return -EINVAL;
    }
    if (!current_proc) {
        return -ESRCH;
    }
    in = copy_fd((int)SC_ARG(params, 0));
    out = copy_fd(fd_out);
    if (!in || !out) {
        return -EBADF;
    }
    rc = copy_regular(in);
    if (rc == 0) {
        rc = copy_regular(out);
    }
    if (rc != 0) {
        return rc;
    }
    if (!(in->flags & FD_FLAG_READ) || !(out->flags & FD_FLAG_WRITE) ||
        (out->ofd->status_flags & OFD_APPEND)) {
        return -EBADF;
    }
    pos_in = in->ofd->offset;
    pos_out = out->ofd->offset;
    rc = uoff_in ? copy_off_get(uoff_in, 0, &pos_in) : 0;
    if (rc == 0 && uoff_out) {
        rc = copy_off_get(uoff_out, 0, &pos_out);
    }
    if (rc != 0) {
        return rc;
    }
    if (pos_in + len > COPY_POS_MAX || pos_out + len > COPY_POS_MAX) {
        return -EOVERFLOW;
    }
    rc = vfs_getattr(in->ofd->vmnt, in->ofd->vnode, &st);
    if (rc != 0) {
        return rc;
    }
    if (pos_in >= st.size) {
        len = 0;
    } else if (len > st.size - pos_in) {
        len = (uint32_t)(st.size - pos_in);
    }
    if (copy_same_file(in, out) && pos_out < pos_in + len && pos_in < pos_out + len) {
        return -EINVAL;
    }
    if (len == 0) {
        return 0;
    }
    buf = copy_buf_get();
    if (!buf) {
        return -ENOMEM;
    }
    rc = copy_from_file(in, &pos_in, out, fd_out, &pos_out, len, 0, buf);
    copy_buf_put(buf);
    if (rc > 0 && (copy_pos_done(in, uoff_in, 0, pos_in) != 0 ||
                   copy_pos_done(out, uoff_out, 0, pos_out) != 0)) {
        return -EFAULT;
    }
    return rc;
}

/* fallocate's modes, of which one at most is given, and its one flag. */
#define FALLOC_FL_KEEP_SIZE      0x01u
#define FALLOC_FL_PUNCH_HOLE     0x02u
#define FALLOC_FL_COLLAPSE_RANGE 0x08u
#define FALLOC_FL_ZERO_RANGE     0x10u
#define FALLOC_FL_INSERT_RANGE   0x20u
#define FALLOC_FL_UNSHARE_RANGE  0x40u
#define FALLOC_FL_WRITE_ZEROES   0x80u
#define FALLOC_FL_MODE_MASK                                                   \
    (FALLOC_FL_PUNCH_HOLE | FALLOC_FL_COLLAPSE_RANGE | FALLOC_FL_ZERO_RANGE | \
     FALLOC_FL_INSERT_RANGE | FALLOC_FL_UNSHARE_RANGE | FALLOC_FL_WRITE_ZEROES)

/** Whether Linux takes fallocate mode `mode` (vfs_fallocate), 1 or 0: one
 * mode at most, a hole punched with the size kept, and a range collapsed,
 * inserted or zeroed by writes without it. The caller answers 0 with
 * -EOPNOTSUPP. */
static int32_t fallocate_mode_ok(uint32_t mode)
{
    uint32_t kind = mode & FALLOC_FL_MODE_MASK;

    if (mode & ~(FALLOC_FL_MODE_MASK | FALLOC_FL_KEEP_SIZE)) {
        return 0;
    }
    if (kind == 0 || kind == FALLOC_FL_UNSHARE_RANGE || kind == FALLOC_FL_ZERO_RANGE) {
        return 1;
    }
    if (kind == FALLOC_FL_PUNCH_HOLE) {
        return (mode & FALLOC_FL_KEEP_SIZE) != 0;
    }
    if (kind == FALLOC_FL_COLLAPSE_RANGE || kind == FALLOC_FL_INSERT_RANGE ||
        kind == FALLOC_FL_WRITE_ZEROES) {
        return (mode & FALLOC_FL_KEEP_SIZE) == 0;
    }
    return 0;
}

/** The file type, as st_mode has it, of what `entry` is open on, for
 * fallocate: a file or a /proc file is regular, a directory of a file
 * system or of /dev, /proc or /sys a directory, a pipe a FIFO, a device as
 * devfs gives its mode, and anything else, a socket or a terminal among
 * them, no file at all (0). */
static uint32_t fallocate_type(fd_entry_t *entry)
{
    if (entry->type == FD_TYPE_FILE || entry->type == FD_TYPE_PROC) {
        return S_IFREG;
    }
    if (entry->type == FD_TYPE_DIR || entry->type == FD_TYPE_SYNTH_DIR) {
        return S_IFDIR;
    }
    if (entry->type == FD_TYPE_PIPE) {
        return S_IFIFO;
    }
    if (entry->type == FD_TYPE_DEVICE) {
        return devfs_fd_mode(entry) & S_IFMT;
    }
    return 0;
}

/**
 * The body of fallocate(2), which `ksys_fallocate` locks: 0 or a negative
 * errno, by Linux's vfs_fallocate's checks in its order and then the
 * type's operation under the description's lock and the file's
 * (`file_lock`). The type checks RLIMIT_FSIZE itself (`vfs_newsize_ok`)
 * once it has taken the mode, as Linux's shmem_fallocate does
 * (mm/shmem.c:3459-3498 in 6.12), so that a mode or a type that takes no
 * allocation is -EOPNOTSUPP with no SIGXFSZ. The file's lock is held from
 * before `vfs_fallocate` asks whether a loop device stands on the file to
 * the end of the change, since an attachment publishes itself under that
 * lock (`loop_attach`): a check made before it could pass a file attached
 * while the change waited for the lock.
 */
static long fallocate_fd(int fd, uint32_t mode, uint64_t offset, uint64_t len)
{
    fd_entry_t *entry;
    struct vfs_ref file;
    uint32_t type;
    int32_t rc;
    int own;

    if (!current_proc) {
        return -ESRCH;
    }
    entry = fd_get(current_proc->fds, fd);
    if (!entry || (entry->flags & FD_FLAG_PATH)) {
        return -EBADF;
    }
    if ((offset >> 63) != 0 || len == 0 || (len >> 63) != 0) {
        return -EINVAL;
    }
    if (!fallocate_mode_ok(mode)) {
        return -EOPNOTSUPP;
    }
    if (!(entry->flags & FD_FLAG_WRITE)) {
        return -EBADF;
    }
    type = fallocate_type(entry);
    if (type == S_IFIFO) {
        return -ESPIPE;
    }
    if (type == S_IFDIR) {
        return -EISDIR;
    }
    if (type != S_IFREG && type != S_IFBLK) {
        return -ENODEV;
    }
    if (((offset + len) >> 63) != 0) {
        return -EFBIG;
    }
    if (type != S_IFREG || entry->type != FD_TYPE_FILE || !entry->ofd || !entry->ofd->vmnt) {
        return -EOPNOTSUPP;
    }
    fd_ofd_lock(entry->ofd);
    own = file_lock(entry->ofd, &file);
    rc = vfs_fallocate(entry->ofd->vmnt, entry->ofd->fhandle, mode, offset, len);
    file_unlock(&file, own);
    fd_ofd_unlock(entry->ofd);
    return rc;
}

/** fallocate(2) under the big lock unless the descriptor names a file
 * `file_vfs_writes_unlocked` admits, whose type then runs it under the
 * file's lock (`fd_change_lock`). Returns as `fallocate_fd` does. */
long ksys_fallocate(int fd, uint32_t mode, uint64_t offset, uint64_t len)
{
    int big = fd_change_lock(fd);
    long rc = fallocate_fd(fd, mode, offset, len);

    big_give(big);
    return rc;
}

/**
 * fallocate(2) as a 64-bit architecture has it: the descriptor, the mode,
 * the offset and the length, one register each. i386's, which splits the
 * offset and the length, is in arch/i686/sys_ia32.c.
 */
long sys_fallocate(isr_params_t *params)
{
    return ksys_fallocate((int)SC_ARG(params, 0), (uint32_t)SC_ARG(params, 1),
                          (uint64_t)SC_ARG(params, 2), (uint64_t)SC_ARG(params, 3));
}

/**
 * Open the file `node` of `mnt`, made with one hold for the caller, for
 * reading and writing on a new descriptor of the current process,
 * close-on-exec when `cloexec`: the descriptor, or a negative errno with the
 * hold put back.
 */
static long memfd_open(struct vfs_mount *mnt, void *node, int cloexec)
{
    struct vfs_stat st;
    fd_entry_t *e;
    int fd = fd_alloc(current_proc->fds);

    if (fd < 0 || vfs_open_node(mnt, node, 2, &fd_slot(current_proc->fds, fd)->ofd->fhandle) != 0) {
        if (fd >= 0) {
            fd_free(current_proc->fds, fd);
        }
        mnt->type->ops->put(mnt, node);
        return fd < 0 ? fd : -ENOMEM;
    }
    e = fd_slot(current_proc->fds, fd);
    e->type = FD_TYPE_FILE;
    e->node = NULL;
    e->flags = FD_FLAG_READ | FD_FLAG_WRITE | (cloexec ? FD_FLAG_CLOEXEC : 0u);
    e->ofd->vmnt = mnt;
    vfs_wb_err_sample(mnt, &e->ofd->wb_seen, &e->ofd->sb_seen);
    e->ofd->vnode = node;
    e->ofd->ino = vfs_getattr(mnt, node, &st) == 0 ? st.ino : 0;
    e->ofd->ino_seen = vfs_ino_err_sample(mnt, e->ofd->ino);
    vfs_desc_open(mnt);
    fd_install(current_proc->fds, fd);
    return fd;
}

/* The longest memfd name: Linux's NAME_MAX less the "memfd:" of its link. */
#define MFD_NAME_MAX 249

/**
 * memfd_create(2) takes name and flags.
 *
 * A file in memory that no path names, on the tmpfs `vfs_memfd_mount` has,
 * open for reading and writing on a new descriptor: mode 0777 and the
 * caller's owner, 0666 with MFD_NOEXEC_SEAL; sealed against sealing unless
 * MFD_ALLOW_SEALING or MFD_NOEXEC_SEAL, and sealed against a change of its
 * execute bits with MFD_NOEXEC_SEAL; close-on-exec with MFD_CLOEXEC. It goes
 * with its descriptions' last close, and /proc/PID/fd names it
 * "/memfd:NAME (deleted)". Returns the descriptor or a negative errno: a
 * name longer than 249 bytes, MFD_HUGETLB or another unknown flag, and
 * MFD_NOEXEC_SEAL with MFD_EXEC are EINVAL; a name the caller cannot reach
 * is EFAULT, a full descriptor table EMFILE, and the rest ENOMEM or
 * `tmpfs_memfd`'s.
 */
long sys_memfd_create(isr_params_t *params)
{
    uint32_t flags = (uint32_t)SC_ARG(params, 1);
    struct vfs_mount *mnt = vfs_memfd_mount();
    uint32_t mode = 0777u, seals = TMPFS_SEAL_SEAL;
    char name[MFD_NAME_MAX + 2];
    void *node;
    int32_t rc;

    if (!current_proc) {
        return -ESRCH;
    }
    if ((flags & ~(MFD_CLOEXEC | MFD_ALLOW_SEALING | MFD_NOEXEC_SEAL | MFD_EXEC)) ||
        ((flags & MFD_NOEXEC_SEAL) && (flags & MFD_EXEC))) {
        return -EINVAL;
    }
    rc = strncpy_from_user(name, (const char __user *)SC_ARG(params, 0), sizeof(name));
    if (rc > MFD_NAME_MAX || rc == -ENAMETOOLONG) {
        return -EINVAL;
    }
    if (rc < 0) {
        return rc;
    }
    if (flags & MFD_NOEXEC_SEAL) {
        mode = 0666u;
        seals = TMPFS_SEAL_EXEC;
    } else if (flags & MFD_ALLOW_SEALING) {
        seals = 0;
    }
    if (!fd_can_alloc(current_proc->fds)) {
        return -EMFILE;
    }
    rc = tmpfs_memfd(mnt, name, mode, seals, &node);
    if (rc != 0) {
        return rc;
    }
    return memfd_open(mnt, node, (flags & MFD_CLOEXEC) != 0);
}

/* memfd_secret's one flag, O_CLOEXEC. */
#define SECRET_CLOEXEC 0x80000u

/**
 * memfd_secret(2) takes flags.
 *
 * A file in memory whose pages only its shared mappings reach, on the tmpfs
 * `vfs_memfd_mount` has (`tmpfs_secret_file`), open for reading and writing
 * on a new descriptor, close-on-exec with O_CLOEXEC; /proc/PID/fd names it
 * "/secretmem (deleted)". Linux takes its pages out of the kernel's own
 * mapping of memory; here the kernel's mapping keeps them, and only the
 * paths that reach a page through another process or a device are refused.
 * Returns the descriptor or a negative errno: another flag is EINVAL, a full
 * descriptor table EMFILE, and the rest ENOMEM.
 */
long sys_memfd_secret(isr_params_t *params)
{
    uint32_t flags = (uint32_t)SC_ARG(params, 0);
    struct vfs_mount *mnt = vfs_memfd_mount();
    void *node;
    int32_t rc;

    if (!current_proc) {
        return -ESRCH;
    }
    if (flags & ~SECRET_CLOEXEC) {
        return -EINVAL;
    }
    if (!mnt) {
        return -ENOMEM;
    }
    if (!fd_can_alloc(current_proc->fds)) {
        return -EMFILE;
    }
    rc = tmpfs_secret_file(mnt, &node);
    if (rc != 0) {
        return rc;
    }
    return memfd_open(mnt, node, (flags & SECRET_CLOEXEC) != 0);
}

/**
 * Whether the caller may count the cached pages of what `e` is open on, as
 * Linux's can_do_cachestat has it since a fix that 6.12's stable kernels
 * carry: the descriptor is open for writing, or the caller owns the file
 * by its file-system user id or has privilege over files, as Linux's
 * inode_owner_or_capable judges, or may write it by its mode.
 */
static int cachestat_allowed(fd_entry_t *e)
{
    struct kstat st;

    if (e->flags & FD_FLAG_WRITE) {
        return 1;
    }
    if (fstat_fill(e, &st) != 0) {
        return 0;
    }
    return (uint32_t)st.st_uid == proc_fsuid(current_proc) || cred_fs_privileged() ||
           access_fd(e, W_OK, AT_EACCESS) == 0;
}

/**
 * cachestat(2) takes fd, a range, a record and flags.
 *
 * The counts of the pages of the file `fd` is open on, in the range of
 * Linux's struct cachestat_range, an offset and a length in bytes, a length
 * of 0 reaching to the end, that are in memory and dirty there
 * (`vfs_cachestat`), copied out as Linux's struct cachestat; the layouts are
 * the same for an i386 program. What is not a file of a mounted file system
 * counts no page. Returns 0 or a negative errno, in Linux 6.12's order: a
 * descriptor that is not open, or is an O_PATH one, is EBADF; a range that
 * cannot be read EFAULT; a caller `cachestat_allowed` refuses EPERM; flags
 * not 0 EINVAL; a record that cannot be written EFAULT.
 */
long sys_cachestat(isr_params_t *params)
{
    fd_entry_t *e;
    uint64_t range[2], first, last;
    struct vfs_cachestat cs;

    if (!current_proc) {
        return -ESRCH;
    }
    e = fd_get(current_proc->fds, (int)SC_ARG(params, 0));
    if (!e || (e->flags & FD_FLAG_PATH)) {
        return -EBADF;
    }
    if (copy_from_user(range, (const void __user *)SC_ARG(params, 1), sizeof(range)) != 0) {
        return -EFAULT;
    }
    if (!cachestat_allowed(e)) {
        return -EPERM;
    }
    if ((uint32_t)SC_ARG(params, 3) != 0) {
        return -EINVAL;
    }
    first = range[0] >> 12;
    last = range[1] == 0 ? ~(uint64_t)0 : (range[0] + range[1] - 1u) >> 12;
    k_memset(&cs, 0, sizeof(cs));
    if ((e->type == FD_TYPE_FILE || e->type == FD_TYPE_DIR) && e->ofd && e->ofd->vmnt &&
        e->ofd->vnode) {
        fd_ofd_lock(e->ofd);
        vfs_cachestat(e->ofd->vmnt, e->ofd->vnode, first, last, &cs);
        fd_ofd_unlock(e->ofd);
    }
    return copy_to_user((void __user *)SC_ARG(params, 2), &cs, sizeof(cs)) != 0 ? -EFAULT : 0;
}

/* flock(2)'s obsolete mandatory-lock bit, which Linux accepts and ignores. */
#define LOCK_MAND 32

/**
 * flock(2) takes fd and operation.
 *
 * LOCK_SH, LOCK_EX or LOCK_UN, with LOCK_NB for no wait, over the whole of
 * what `fd` is open on, owned by the open file description, as
 * lock.Readme.md describes: a dup or a fork shares it, and the last close of
 * the description lets it go. A lock of the other kind the description holds
 * is let go first, so a conversion is not atomic, as on Linux. LOCK_MAND is
 * accepted and ignored, as Linux does. Returns 0; EINVAL for another
 * operation; EBADF for a descriptor that is not open, is an O_PATH one, or
 * is not open for reading or writing when it asks for a lock; EWOULDBLOCK
 * when LOCK_NB meets a conflict; EINTR when a signal ends the wait; or
 * ENOLCK.
 *
 * The call runs without the big lock, as Linux's flock takes none: the
 * locks have their own (lock.c), and the big lock is taken only for a file
 * keyed by its node (`fd_locks_by_node`).
 */
long sys_flock(isr_params_t *params)
{
    int op = (int)SC_ARG(params, 1);
    int kind = op & ~LOCK_NB;
    fd_entry_t *e;
    struct lock_key key;
    int32_t rc;

    if (op & LOCK_MAND) {
        return 0;
    }
    if (kind != LOCK_SH && kind != LOCK_EX && kind != LOCK_UN) {
        return -EINVAL;
    }
    if (!current_proc) {
        return -ESRCH;
    }
    e = fd_get(current_proc->fds, (int)SC_ARG(params, 0));
    if (!e || !e->ofd || (e->flags & FD_FLAG_PATH) ||
        (kind != LOCK_UN && !(e->flags & (FD_FLAG_READ | FD_FLAG_WRITE)))) {
        return -EBADF;
    }
    if (!fd_locks_by_node(e)) {
        fd_lock_key(e, &key);
        return lock_flock(&key, e->ofd, op);
    }
    bkl_enter();
    fd_lock_key(e, &key);
    rc = lock_flock(&key, e->ofd, op);
    bkl_exit();
    return rc;
}

/** F_SETFL on the held descriptor `entry`: set the O_APPEND and O_NONBLOCK
 * bits of `arg` on its description, in one atomic step
 * (`fd_ofd_status_set`). 0, or -EPERM for a change of O_APPEND on an
 * append-only file, which Linux's setfl refuses either way. */
static long fcntl_setfl(fd_entry_t *entry, uintptr_t arg)
{
    if (!entry->ofd) {
        return 0;
    }
    if (((arg ^ entry->ofd->status_flags) & OFD_APPEND) && ofd_append_only(entry->ofd)) {
        return -EPERM;
    }
    fd_ofd_status_set(entry->ofd, OFD_APPEND | OFD_NONBLOCK, arg & (OFD_APPEND | OFD_NONBLOCK));
    return 0;
}

/* fcntl's commands besides the record locks' and the seals'. */
enum {
    F_DUPFD = 0,
    F_GETFD = 1,
    F_SETFD = 2,
    F_GETFL = 3,
    F_SETFL = 4,
    F_DUPFD_CLOEXEC = 1030,
    F_SETPIPE_SZ = 1031,
    F_GETPIPE_SZ = 1032
};

/**
 * Whether fcntl's command `cmd` on the held descriptor `entry` needs the big
 * lock, which every command Linux's do_fcntl runs without one
 * (fs/fcntl.c:446-528) does not: F_GETFD and F_GETFL read a word, F_SETFD
 * changes the descriptor under the table's lock (`fd_set_flags`), as
 * Linux's set_close_on_exec under `file_lock` (fs/file.c:1183-1194), F_SETFL
 * changes the description in one atomic step, F_DUPFD and F_DUPFD_CLOEXEC
 * take a slot and publish it under the table's lock with a reference on the
 * description, the record locks have their own (`fcntl_lock`), and
 * F_GETPIPE_SZ reads a word. The big lock is still taken for F_SETFL on a
 * file whose attributes need it, which `ofd_append_only` reads, for a
 * duplicate of a socket, whose reference count it guards, or of a file of
 * the KRFFS root, whose holders it counts, and for the seals and
 * F_SETPIPE_SZ, which charge and change what it guards.
 */
static int fcntl_big(const fd_entry_t *entry, int cmd)
{
    switch (cmd) {
    case F_SETFL:
        return entry->ofd && entry->ofd->vmnt && !file_vfs_unlocked(entry);
    case F_DUPFD:
    case F_DUPFD_CLOEXEC:
        return entry->type == FD_TYPE_SOCKET || entry->node != NULL;
    case F_ADD_SEALS:
    case F_GET_SEALS:
    case F_SETPIPE_SZ:
        return 1;
    default:
        return 0;
    }
}

static long fcntl_cmd(fd_entry_t *entry, int cmd, uintptr_t arg);

/**
 * fcntl64(2) on i386 and fcntl(2) on x86-64: the descriptor, the command
 * and its argument. Returns what the command gives or a negative errno:
 * -EBADF for a descriptor that is not open, -EINVAL for a command it does
 * not take.
 *
 * F_GETFD and F_SETFD read and set the close-on-exec bit, which belongs to
 * the descriptor. F_GETFL reports the access mode, 0, 1 or 2 for O_RDONLY,
 * O_WRONLY and O_RDWR, with O_APPEND, O_NONBLOCK, O_SYNC and O_DSYNC, and
 * O_PATH for an O_PATH descriptor. F_SETFL sets O_APPEND and O_NONBLOCK,
 * which belong to the open file description a dup or a fork shares, and
 * ignores the other bits, the access mode, O_CLOEXEC and the sync flags
 * among them, as POSIX allows and Linux's setfl does; it is
 * -EPERM when it would change O_APPEND of an append-only file, as Linux's
 * setfl refuses either way.
 *
 * F_DUPFD and F_DUPFD_CLOEXEC duplicate onto the lowest free descriptor at
 * or above `arg`, as POSIX requires. The description the allocation gave
 * the new slot is dropped, and the duplicate shares the source's, and so
 * the offset, with a reference to it and to a socket; it is close-on-exec
 * only with F_DUPFD_CLOEXEC. An `arg` at or past the descriptor limit is
 * -EINVAL, since the table grows to the limit and no further; no free slot
 * at or above it is -EMFILE, and no room in the heap -ENOMEM. The
 * allocation may move the table's entries, so the source is looked up again
 * after it.
 *
 * The record locks are `fcntl_lock`'s and the seals `fcntl_seals`'s.
 * F_GETPIPE_SZ gives a pipe's capacity, which bash asks before it writes a
 * here-document into a pipe it reads itself, and F_SETPIPE_SZ sets it
 * (`pipe_set_size`), as GNU cat asks before it splices; both are -EBADF
 * for anything else. An O_PATH descriptor takes the commands Linux lets it
 * take, the two duplications, F_GETFD, F_SETFD and F_GETFL, and is -EBADF to
 * the rest.
 *
 * The call runs without the big lock, and takes it only for the commands
 * and descriptors `fcntl_big` names.
 */
long sys_fcntl64(isr_params_t *params)
{
    int fd = (int)SC_ARG(params, 0);
    int cmd = (int)SC_ARG(params, 1);
    uintptr_t arg = SC_ARG(params, 2);
    fd_entry_t *entry;
    long rc;

    if (!current_proc) {
        return -ESRCH;
    }
    entry = fd_get(current_proc->fds, fd);
    if (!entry) {
        return -EBADF;
    }
    if ((entry->flags & FD_FLAG_PATH) && cmd != F_DUPFD && cmd != F_DUPFD_CLOEXEC &&
        cmd != F_GETFD && cmd != F_SETFD && cmd != F_GETFL) {
        return -EBADF;
    }
    if (bkl_held() || !fcntl_big(entry, cmd)) {
        return fcntl_cmd(entry, cmd, arg);
    }
    bkl_enter();
    rc = fcntl_cmd(entry, cmd, arg);
    bkl_exit();
    return rc;
}

/**
 * fcntl's command `cmd` with its argument `arg` on the held descriptor
 * `entry`, which `sys_fcntl64` checked, as that function describes it.
 */
static long fcntl_cmd(fd_entry_t *entry, int cmd, uintptr_t arg)
{
    switch (cmd) {
    case F_GETFD:
        return (entry->flags & FD_FLAG_CLOEXEC) ? 1 : 0; /* 1 == FD_CLOEXEC */
    case F_SETFD:
        fd_set_flags(current_proc->fds, entry, FD_FLAG_CLOEXEC, (arg & 1) ? FD_FLAG_CLOEXEC : 0u);
        return 0;
    case F_GETFL: {
        int rw = entry->flags & (FD_FLAG_READ | FD_FLAG_WRITE);
        int fl;
        if (rw == (FD_FLAG_READ | FD_FLAG_WRITE)) {
            fl = 2;
        } else if (rw == FD_FLAG_WRITE) {
            fl = 1;
        } else {
            fl = 0;
        }
        if (entry->ofd) {
            fl |= (int)(entry->ofd->status_flags & (OFD_APPEND | OFD_NONBLOCK | OFD_SYNC));
        }
        if (entry->flags & FD_FLAG_PATH) {
            fl |= O_PATH;
        }
        return fl;
    }
    case F_SETFL:
        return fcntl_setfl(entry, arg);
    case F_DUPFD:
    case F_DUPFD_CLOEXEC: {
        int newfd;

        if (arg >= (uintptr_t)fd_limit(current_proc->fds)) {
            return -EINVAL;
        }
        newfd = fd_alloc_from(current_proc->fds, (int)arg);
        if (newfd < 0) {
            return newfd;
        }
        dup_fill(fd_slot(current_proc->fds, newfd), entry, cmd == F_DUPFD_CLOEXEC);
        fd_install(current_proc->fds, newfd);
        return newfd;
    }
    case F_GETLK:
    case F_SETLK:
    case F_SETLKW:
    case F_GETLK64:
    case F_SETLK64:
    case F_SETLKW64:
    case F_OFD_GETLK:
    case F_OFD_SETLK:
    case F_OFD_SETLKW:
        return fcntl_lock(entry, cmd, arg);
    case F_ADD_SEALS:
    case F_GET_SEALS:
        return fcntl_seals(entry, cmd, (uint32_t)arg);
    case F_GETPIPE_SZ:
        return pipe_is(entry) ? pipe_get_size(entry) : -EBADF;
    case F_SETPIPE_SZ:
        return pipe_is(entry) ? pipe_set_size(entry, (uint32_t)arg, proc_privileged(current_proc))
                              : -EBADF;
    default:
        return -EINVAL;
    }
}

#ifdef ARCH_WANT_OLD_STAT
/**
 * Copy the three times out of a kstat into the narrower old stat, a
 * structure of its own, which every path that fills it fills separately.
 */
static void stat_times_from_stat64(struct linux_stat *kst, const struct kstat *s64)
{
    kst->st_atime = (unsigned long)s64->st_atime;
    kst->st_atime_nsec = s64->st_atime_nsec;
    kst->st_mtime = (unsigned long)s64->st_mtime;
    kst->st_mtime_nsec = s64->st_mtime_nsec;
    kst->st_ctime = (unsigned long)s64->st_ctime;
    kst->st_ctime_nsec = s64->st_ctime_nsec;
}

/** The three times of a KRFFS node, into the old stat. */
static void stat_node_times(struct linux_stat *kst, const krffs_node_t *node)
{
    struct kstat s64;

    stat64_node_times(&s64, node);
    stat_times_from_stat64(kst, &s64);
}

/** Copy the old stat `kst` out to `st`, or answer -ENOTDIR when the path
 * named a directory by its end (`path_names_dir`) and reached something
 * else, as Linux's walk does. */
static int32_t old_stat_put(struct linux_stat __user *st, const struct linux_stat *kst, int dir)
{
    struct linux_stat seen = *kst;

    if (dir && (kst->st_mode & S_IFMT) != S_IFDIR) {
        return -ENOTDIR;
    }
    seen.st_uid = (unsigned short)uid_seen(kst->st_uid);
    seen.st_gid = (unsigned short)gid_seen(kst->st_gid);
    return copy_to_user(st, &seen, sizeof(seen)) != 0 ? -EFAULT : 0;
}

/** The root directory's three times, into the old stat. */
static void stat_root_times(struct linux_stat *kst)
{
    struct kstat s64;

    stat64_root_times(&s64);
    stat_times_from_stat64(kst, &s64);
}

/**
 * The core of the old stat(2) and lstat(2): the file the user path `path`
 * names, relative to the working directory, into the old `struct linux_stat`
 * at `st`, a final link followed when `follow` or when the path named a
 * directory by its end. Returns 0 or a negative errno: -EFAULT for a NULL
 * pointer or a copy that fails, -ENOTDIR for a path that named a directory
 * by its end and reached something else, or the walk's errors, -ENOENT,
 * -ENOTDIR and -EACCES among them.
 *
 * "/", for a process without a root of its own (`names_tree_root`) on a
 * KRFFS boot root, is reported without a walk as that root directory: mode
 * 0755, inode 1, owner 0, and the root's stored times, which a name made in
 * it stamps, so a program watching the directory sees the change. On a root
 * of another type, ext2 among them, "/" is walked like any other path. A
 * file of /dev, /proc or /sys, or of another mounted type, is filled from
 * the stat64 the wider calls give, narrowed. The walk keeps its mount's lock
 * until the node is read, as `do_stat64`'s does. A KRFFS root file has a
 * link count of 1, since KRFFS has no hard links.
 */
static int32_t do_old_stat(const char __user *path, struct linux_stat __user *st, int follow)
{
    struct linux_stat kst;
    char *pathbuf;
    krffs_node_t *node;
    int dir = 0;
    int len;

    if (!current_proc || !path || !st) {
        return -EFAULT;
    }

    len = path_from_user_dir(AT_FDCWD, path, &pathbuf, &dir);
    if (len != 0) {
        return len;
    }
    follow |= dir;

    k_memset(&kst, 0, sizeof(kst));

    if (names_tree_root(pathbuf) && vfs_root_mount()->type == &vfs_krffs_type) {
        path_buf_put(pathbuf);
        kst.st_mode = S_IFDIR | 0755; /* drwxr-xr-x */
        kst.st_nlink = 1;
        kst.st_ino = 1;
        kst.st_blksize = 4096;
        stat_root_times(&kst);
        return old_stat_put(st, &kst, dir);
    }

    {
        struct vfs_ref ref;
        const struct vpath *svp;
        int32_t rc =
            vfs_resolve(0, pathbuf, VFS_WALK_LOCK | (follow ? 0 : VFS_WALK_NOFOLLOW), &ref, 0, 0);

        path_buf_put(pathbuf);
        if (rc != 0) {
            return rc;
        }
        svp = vfs_synth_vpath(ref.mnt, ref.node);
        if (svp) {
            struct kstat s64;

            rc = synth_stat(svp, &s64, follow);
            vfs_walk_unlock(ref.mnt);
            vfs_ref_release(&ref);
            if (rc != 0) {
                return rc;
            }
            kst.st_mode = (unsigned short)s64.st_mode;
            kst.st_nlink = (unsigned short)s64.st_nlink;
            kst.st_dev = (unsigned long)s64.st_dev;
            kst.st_ino = (unsigned long)s64.st_ino;
            kst.st_rdev = (unsigned long)s64.st_rdev;
            kst.st_blksize = (unsigned long)s64.st_blksize;
            stat_times_from_stat64(&kst, &s64);
            return old_stat_put(st, &kst, dir);
        }
        if (!mnt_is_root(ref.mnt)) {
            struct kstat s64;

            rc = vfs_fill_stat64(&s64, &ref);
            vfs_walk_unlock(ref.mnt);
            vfs_ref_release(&ref);
            if (rc != 0) {
                return rc;
            }
            kst.st_mode = (unsigned short)s64.st_mode;
            kst.st_nlink = (unsigned short)s64.st_nlink;
            kst.st_dev = (unsigned long)s64.st_dev;
            kst.st_ino = (unsigned long)s64.st_ino;
            kst.st_uid = (unsigned short)s64.st_uid;
            kst.st_gid = (unsigned short)s64.st_gid;
            kst.st_rdev = (unsigned long)s64.st_rdev;
            kst.st_size = (unsigned long)s64.st_size;
            kst.st_blksize = (unsigned long)s64.st_blksize;
            kst.st_blocks = (unsigned long)s64.st_blocks;
            stat_times_from_stat64(&kst, &s64);
            return old_stat_put(st, &kst, dir);
        }
        node = ref.node;
        /* '..' can reach the root, whose stored mode may lack S_IFDIR. */
        kst.st_mode = node == krffs_fs->node
                          ? S_IFDIR | 0755
                          : (unsigned short)(krffs_mode_type(node->mode) | (node->mode & 07777));
        kst.st_nlink = 1;
        kst.st_uid = (unsigned short)node->uid;
        kst.st_gid = (unsigned short)node->gid;
        kst.st_size = (unsigned long)node->data_size;
        kst.st_blksize = 4096;
        kst.st_blocks = (node->data_size + 511) / 512;
        kst.st_ino = (unsigned long)krffs_node_ino(node);
        stat_node_times(&kst, node);
        vfs_walk_unlock(ref.mnt);
        vfs_ref_release(&ref);
    }
    return old_stat_put(st, &kst, dir);
}

/**
 * The old stat(2), 106 on i386: the path and the old stat, a final symbolic
 * link followed (`do_old_stat`). musl maps SYS_stat and SYS_lstat to the
 * 64-bit forms, 195 and 196, so this call and `sys_lstat` serve a caller
 * that issues the old numbers itself.
 */
long sys_stat(isr_params_t *params)
{
    return do_old_stat((const char __user *)SC_ARG(params, 0),
                       (struct linux_stat __user *)SC_ARG(params, 1), 1);
}

/** The old lstat(2), 107 on i386: `sys_stat` with a final link not
 * followed. */
long sys_lstat(isr_params_t *params)
{
    return do_old_stat((const char __user *)SC_ARG(params, 0),
                       (struct linux_stat __user *)SC_ARG(params, 1), 0);
}
#endif /* ARCH_WANT_OLD_STAT */

/**
 * creat(2): the path, resolved from the working directory, and the mode.
 * Returns a descriptor open for writing, or a negative errno. The call is
 * open(2) with O_WRONLY, O_CREAT and O_TRUNC, made through the same code
 * (`open_named`), so the -EISDIR for a directory, the root included, the
 * write check, the truncation of an existing file with its set-id clear,
 * and the stamp of a new node are open's.
 */
long sys_creat(isr_params_t *params)
{
    const char __user *path = (const char __user *)SC_ARG(params, 0);
    uint32_t mode = (uint32_t)SC_ARG(params, 1);
    char *pathbuf;
    int end;
    int32_t rc;

    if (!current_proc || !path) {
        return -EFAULT;
    }
    rc = path_from_user_dir(AT_FDCWD, path, &pathbuf, &end);
    if (rc != 0) {
        return rc;
    }
    rc = open_named(path_name(pathbuf), 1 /* O_WRONLY */ | O_CREAT | O_TRUNC, mode, end);
    path_buf_put(pathbuf);
    return rc;
}

/**
 * Remove the name `leaf` from the KRFFS root's directory `parent`: the
 * root's half of `unlink_named`, run under the root's namespace lock, which
 * the caller took with its walk and keeps until this returns. Returns 0 or a
 * negative errno.
 *
 * The directory's write bit is checked before the name is looked up, so a
 * caller who may not write there sees -EACCES and does not learn whether
 * the name exists. The root and a directory are -EISDIR, the root known by
 * its node, since its stored mode may lack the directory bits, and anything
 * else -ENOTDIR when the path had a slash after the name (`slash`); the sticky
 * rule may answer -EPERM, and a file a loop device stands on is -EBUSY: the
 * holder count below is the descriptor tables, which an attachment is not
 * in, and the root has no count of its own to keep the extent.
 *
 * The holder count is the last decision, because keeping the node bypasses
 * the refusals the file system would have made: with a holder the node
 * loses its name and stays until the last close; with none it is freed
 * now, since only a last close frees a kept node and a node nobody holds
 * would never be freed. The count is right only under the lock. An open
 * holds it from its walk until it has published its descriptor, so the
 * count sees every descriptor an open has made, and an open that walks
 * after the removal finds no name.
 */
static int32_t unlink_root_name(const struct vfs_ref *parent, const char *leaf, int slash)
{
    krffs_node_t *node;
    void *found;
    int32_t rc = cred_access_node((krffs_node_t *)parent->node, CRED_W, 0);

    if (rc != 0) {
        return rc;
    }
    rc = parent->mnt->type->ops->lookup(parent->mnt, parent->node, leaf, &found);
    if (rc != 0) {
        return rc;
    }
    node = found;
    if (node == krffs_fs->node || KRFFS_IS_DIR(node->mode)) {
        return -EISDIR;
    }
    if (slash) {
        return -ENOTDIR;
    }
    rc = cred_sticky_ok((krffs_node_t *)parent->node, node);
    if (rc != 0) {
        return rc;
    }
    if (loop_backs(parent->mnt, node)) {
        return -EBUSY;
    }
    if ((parent->mnt->flags | vfs_mount_base(parent->mnt)->flags) & VFS_MS_RDONLY) {
        return -EROFS;
    }
    if (fd_node_holders(node, NULL) > 0) {
        krffs_unlink_held(node);
        return 0;
    }
    return vfs_unlink_name(parent->mnt, parent->node, leaf);
}

/**
 * Remove the name `path`, an absolute canonical path, which is not a
 * directory: the core of unlink(2) and of unlinkat(2) without AT_REMOVEDIR,
 * `end` saying how the path as written named a directory by its end
 * (`path_names_dir`). Returns 0 or a negative errno.
 *
 * The walk gives -ENOENT for a missing component and -ENOTDIR for a file in
 * the middle of the path. The root, a last name of "." or "..", and any
 * directory are -EISDIR, which tells a caller to use rmdir; an empty path is
 * -EINVAL, a name in /dev, /proc or /sys -EROFS, and a mount point -EBUSY.
 * A path with a slash after its last name, `end` PATH_END_SLASH, names a
 * directory too: a name there that is not one, a symbolic link to one among
 * them, is -ENOTDIR, as on Linux. On the KRFFS root every refusal is made
 * before the core is called (`unlink_root_name`), since `krffs_unlink`
 * answers each with -1, which `kfs_unlink` reports as -EIO; on another type
 * the removal rules are `cred_remove_ref`'s and the rest the type's.
 *
 * The walk keeps the parent's namespace lock, where its type takes one,
 * until the name is gone: on the KRFFS root the holder count of
 * `unlink_root_name` is right only under that lock, which an open holds
 * until it has published its descriptor. On a type that sets `file_locked`
 * the removal holds the directory's lock and then the file's
 * (`vfs_child_lock`), so that the sticky and immutable checks see the file
 * the removal takes, and the permission and the mounts are checked again
 * under them (`dir_recheck`).
 */
static int32_t unlink_named(const char *path, int end)
{
    const char *name;
    struct vfs_ref parent;
    char leaf[VFS_NAME_MAX + 1];
    int32_t rc;

    if (path[0] == '/' && path[1] == '\0') {
        return -EISDIR;
    }

    name = path;
    if (name[0] == '/') {
        name++;
    }

    if (name[0] == '\0') {
        return -EINVAL;
    }
    rc = vfs_resolve(0, path, VFS_WALK_PARENT | VFS_WALK_LOCK, &parent, leaf, sizeof(leaf));
    if (rc != 0) {
        return rc;
    }
    if (leaf_is_dot(leaf)) {
        rc = -EISDIR;
    } else if (vfs_synth_vpath(parent.mnt, parent.node)) {
        rc = end == PATH_END_SLASH ? synth_dev_slash(&parent, leaf)
                                   : synth_dev_remove(&parent, leaf, 0);
    } else if (vfs_leaf_mount(parent.mnt, parent.node, leaf)) {
        rc = -EBUSY;
    } else if (mnt_is_root(vfs_mount_base(parent.mnt))) {
        rc = unlink_root_name(&parent, leaf, end == PATH_END_SLASH);
    } else {
        struct vfs_ref victim;

        vfs_node_lock(&parent);
        rc = vfs_child_lock(&parent, leaf, &victim);
        if (rc == 0) {
            rc = dir_recheck(&parent, leaf, RECHECK_WRITE | RECHECK_MOUNT);
        }
        if (rc == 0 && end == PATH_END_SLASH) {
            rc = leaf_dir_check(&parent, leaf);
            rc = rc == 0 ? -EISDIR : rc;
        } else if (rc == 0) {
            rc = cred_remove_ref(&parent, leaf);
            if (rc == 0) {
                rc = vfs_unlink_name(parent.mnt, parent.node, leaf);
            }
        }
        vfs_child_unlock(&victim);
        vfs_node_unlock(&parent);
    }
    vfs_walk_unlock(parent.mnt);
    vfs_ref_release(&parent);
    return rc;
}

/** `unlink_named` of a path the kernel names itself, which names no
 * directory by its end. */
static int32_t do_unlink(const char *path)
{
    return unlink_named(path, 0);
}

/** unlink(2): the path, resolved from the working directory; -EFAULT for a
 * NULL path, or `unlink_named`'s answer. */
long sys_unlink(isr_params_t *params)
{
    const char __user *upath = (const char __user *)SC_ARG(params, 0);
    char *path;
    int end;
    int32_t rc;

    if (!current_proc || !upath) {
        return -EFAULT;
    }
    rc = path_from_user_dir(AT_FDCWD, upath, &path, &end);
    if (rc != 0) {
        return rc;
    }
    rc = unlink_named(path, end);
    path_buf_put(path);
    return rc;
}

/* Where `fs_core_file` writes: the open file, the place of the next byte,
 * the end of the last bytes written, which a hole leaves short of it, and
 * the count of bytes written, the count RLIMIT_CORE bounds. */
struct core_file {
    struct open_file *ofd;
    uint64_t pos;
    uint64_t end;
    uint64_t written;
};

/** The core file's writer for `core_write`: `len` bytes of `buf` at the
 * file's place, or a hole of `len` bytes for a null `buf`. Returns 0 or the
 * write's error; a short write is a full disk, -ENOSPC. A file a loop
 * device stands on is -EBUSY, asked under the file's lock before each write
 * as write(2) asks: the dump holds no description lock, so another process
 * may attach the file while the dump sleeps between pieces, and an
 * attachment publishes itself under that lock. A SIGKILL sent to
 * the dumping thread, which a SIGKILL to its process during the dump sends
 * it (`sig_complete`), stops the file with -EINTR before each piece, as
 * Linux 6.12.115's `dump_interrupted` stops it (fs/coredump.c:495-499,
 * 859-860, 876-877). */
static int32_t core_file_put(void *ctx, const void *buf, uint32_t len)
{
    struct core_file *cf = ctx;
    struct vfs_ref file;
    int32_t n;
    int big;

    if (current_proc->sig_pending & KSIG_BIT(SIGKILL)) {
        return -EINTR;
    }
    if (buf) {
        file.mnt = cf->ofd->vmnt;
        file.node = cf->ofd->vnode;
        vfs_node_lock(&file);
        if (loop_backs(cf->ofd->vmnt, cf->ofd->vnode)) {
            vfs_node_unlock(&file);
            return -EBUSY;
        }
        n = vfs_write(cf->ofd->vmnt, cf->ofd->fhandle, buf, len, cf->pos);
        if (n > 0 && filemap_disk_mapped(cf->ofd->vmnt, cf->ofd->ino)) {
            big = big_take();
            filemap_disk_written(cf->ofd->vmnt, cf->ofd->ino, cf->pos, buf, (uint32_t)n);
            big_give(big);
        }
        vfs_node_unlock(&file);
        if (n < 0) {
            return n;
        }
        if ((uint32_t)n != len) {
            return -ENOSPC;
        }
        cf->end = cf->pos + len;
        cf->written += len;
    }
    cf->pos += len;
    return 0;
}

/**
 * The core file of the current process (coredump.h), through a descriptor
 * of its table that no other call can reach: it is marked closing and
 * releasing as soon as the open has published it, before anything sleeps,
 * so a process that shares the table cannot write through it, or take a
 * reference to a description the end of the dump frees, and it stays in
 * the table, where the holder rule of the KRFFS root counts it. The slot is
 * taken as it is, not through `fd_get`, whose hold on a shared table no
 * call's end would let go of on the way to a death.
 *
 * A file whose last bytes are a hole is given the byte at its end, a zero,
 * so that it has its whole length. That byte counts toward `limit` as the
 * others do, as Linux counts the byte its dump writes there: a file already
 * at the limit stops short, with -EFBIG.
 */
int32_t fs_core_file(int signo, unsigned long limit)
{
    static const char zero;
    uint64_t most = limit;
    struct core_file cf;
    fd_entry_t *entry;
    uint32_t notes_size;
    uint8_t *notes = core_notes(signo, &notes_size);
    char *path = path_buf_get();
    int32_t fd = -ENOMEM;
    int32_t rc;
    int len;

    if (!notes || !path) {
        goto out;
    }
    k_memcpy(path, "core", 5);
    len = path_prepend(path, VFS_PATH_MAX, 4, proc_cwd(current_proc));
    if (len >= 0) {
        len = canonicalize_abs(path);
    }
    if (len < 0) {
        fd = len;
        goto out;
    }
    (void)do_unlink(path);
    fd = do_open(path_name(path), 1 /* O_WRONLY */ | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
out:
    path_buf_put(path);
    if (fd < 0) {
        kfree(notes);
        return fd;
    }
    entry = fd_slot(current_proc->fds, fd);
    fd_set_flags(current_proc->fds, entry, 0, FD_FLAG_CLOSING | FD_FLAG_RELEASING);
    if (!entry->ofd || !entry->ofd->vmnt) {
        rc = -EINVAL;
    } else {
        cf.ofd = entry->ofd;
        cf.pos = 0;
        cf.end = 0;
        cf.written = 0;
        rc = core_write(core_file_put, &cf, notes, notes_size, most);
        if (rc == 0 && cf.end < cf.pos) {
            cf.pos--;
            rc = cf.written < most ? core_file_put(&cf, &zero, 1) : -EFBIG;
        }
    }
    kfree(notes);
    fd_release_backing(entry);
    fd_free_after(current_proc->fds, fd);
    return rc;
}

/**
 * Make the directory `leaf` in `parent`, the last name of `path`, which the
 * caller's walk reached and whose namespace lock it holds where the type
 * takes one: the body of `do_mkdir`. The lookup that answers -EEXIST
 * (`new_name_taken`) and the creation are one hold of that lock, so a
 * directory of the same name that another process makes while this one
 * waits for the lock is a name that exists here, and not one handed to the
 * file system, which refuses it as it refuses a full volume (ENOSPC on the
 * KRFFS root) or a failing one (EIO on a KRFFS volume). A name that exists
 * in /dev, /proc or /sys is -EEXIST too, before the -EROFS of a name that
 * does not, as on Linux; a directory of /dev takes a new directory
 * (`synth_dev_make`). On a type that sets `file_locked` the walk holds
 * nothing, and the directory's own lock covers the creation alone, with the
 * permission and the mounts checked again under it (`dir_lock`), so a name
 * made meanwhile is the type's -EEXIST. Returns 0 or a negative errno.
 */
static int32_t mkdir_in(const struct vfs_ref *parent, const char *leaf, uint32_t mode,
                        const char *path)
{
    void *found;
    int32_t rc;

    rc = new_name_taken(parent, leaf, path);
    if (rc != 0) {
        return rc;
    }
    if (vfs_synth_vpath(parent->mnt, parent->node)) {
        return synth_dev_make(parent, leaf, mode & 07777u & ~current_proc->fs->umask, NULL);
    }
    if (!mnt_is_root(parent->mnt)) {
        rc = cred_access_ref(parent, CRED_W, 0);
        if (rc == 0) {
            rc = dir_lock(parent, leaf, RECHECK_WRITE | RECHECK_NEW);
            if (rc == 0) {
                rc = vfs_mkdir(parent->mnt, parent->node, leaf,
                               mode & 07777u & ~current_proc->fs->umask);
            }
            vfs_node_unlock(parent);
        }
        return rc;
    }
    rc = cred_access_node((krffs_node_t *)parent->node, CRED_W, 0);
    if (rc != 0) {
        return rc;
    }
    vfs_ns_lock(parent->mnt);
    rc = vfs_mkdir(parent->mnt, parent->node, leaf, mode);
    vfs_ns_unlock(parent->mnt);
    if (rc == 0 && parent->mnt->type->ops->lookup(parent->mnt, parent->node, leaf, &found) == 0) {
        krffs_stamp_new_node(found, (krffs_node_t *)parent->node, mode);
    }
    return rc;
}

/**
 * Create the directory `pathbuf`, an absolute canonical path, with the
 * permission bits of `mode` less the umask, as any creation takes them: the
 * core of mkdir(2) and mkdirat(2). Returns 0 or a negative errno: -EEXIST
 * for the root, which `mkdir -p` walks through, and for any name that
 * exists, a last name of "." or ".." among them; -ENOENT before the root
 * file system is mounted; the walk's errors, -ENOENT for a missing parent
 * and -EACCES for an ancestor the caller may not search among them; or
 * `mkdir_in`'s. The walk keeps the parent's namespace lock, where its type
 * takes one, until the directory is made (`mkdir_in`).
 */
static int32_t do_mkdir(const char *pathbuf, uint32_t mode)
{
    struct vfs_ref parent;
    char leaf[VFS_NAME_MAX + 1];
    int32_t rc;

    if (!vfs_root_mounted()) {
        return -ENOENT;
    }
    if (pathbuf[0] == '/' && pathbuf[1] == '\0') {
        return -EEXIST;
    }
    rc = vfs_resolve(0, pathbuf, VFS_WALK_PARENT | VFS_WALK_LOCK, &parent, leaf, sizeof(leaf));
    if (rc != 0) {
        return rc; /* a missing parent, or a denied ancestor */
    }
    rc = leaf_is_dot(leaf) ? -EEXIST : mkdir_in(&parent, leaf, mode, pathbuf);
    vfs_walk_unlock(parent.mnt);
    vfs_ref_release(&parent);
    return rc;
}

/**
 * mkdir(2): the path, resolved from the working directory, and the mode.
 * -EFAULT for a NULL path, or `do_mkdir`'s answer. The path is copied with
 * a last "." put back (`path_put_dot`), so the walk gives "." as the name
 * and the call answers -EEXIST, as on Linux.
 */
long sys_mkdir(isr_params_t *params)
{
    const char __user *upath = (const char __user *)SC_ARG(params, 0);
    char *path;
    int end;
    int32_t rc;

    if (!current_proc || !upath) {
        return -EFAULT;
    }
    rc = path_from_user_dir(AT_FDCWD, upath, &path, &end);
    if (rc != 0) {
        return rc;
    }
    rc = do_mkdir(path, (uint32_t)SC_ARG(params, 1));
    path_buf_put(path);
    return rc;
}

/** mkdirat(2): the directory descriptor, the path and the mode; `sys_mkdir`
 * with a relative path resolved from `dirfd`, as openat resolves it. */
long sys_mkdirat(isr_params_t *params)
{
    const char __user *upath = (const char __user *)SC_ARG(params, 1);
    char *path;
    int end;
    int32_t rc;

    if (!current_proc || !upath) {
        return -EFAULT;
    }
    rc = path_from_user_dir((int)SC_ARG(params, 0), upath, &path, &end);
    if (rc != 0) {
        return rc;
    }
    rc = do_mkdir(path, (uint32_t)SC_ARG(params, 2));
    path_buf_put(path);
    return rc;
}

/**
 * The first refusal of mknod(2) and mknodat(2), made before the path is
 * copied, as Linux makes it: 0 for a `mode` whose S_IF type bits are none
 * or one of the five kinds, -EPERM for a directory's, and -EINVAL for any
 * other.
 */
static int32_t mknod_kind(uint32_t mode)
{
    uint32_t type = mode & S_IFMT;

    if (type == S_IFDIR) {
        return -EPERM;
    }
    if (type != 0 && type != S_IFREG && type != S_IFCHR && type != S_IFBLK && type != S_IFIFO &&
        type != S_IFSOCK) {
        return -EINVAL;
    }
    return 0;
}

/**
 * Make the node `path` names, an absolute canonical path, of a kind
 * `mknod_kind` has passed: the core of mknod(2) and mknodat(2). Returns 0
 * or a negative errno.
 *
 * The kind is the S_IF type bits of `mode`, a regular file for none; `dev`
 * is a device's number as the call takes it, Linux's 32-bit encoding, which
 * is the kernel's own. The refusals after the kind's and the path's come in
 * Linux's order: the root (EEXIST, `new_name_parent`), a path that named a
 * directory by its end, `end`, as `make_dir_end` answers it, a name that
 * exists (EEXIST, a symbolic link included, `new_name_taken`), then /dev,
 * /proc and /sys, where nothing is made (EROFS), a read-only mount, the
 * write permission on the directory, and last a device node made by a
 * caller who is not root (EPERM). A file system that cannot keep the kind
 * answers EPERM.
 */
static int32_t do_mknod(const char *path, uint32_t mode, uint32_t dev, int end)
{
    uint32_t type = mode & S_IFMT;
    struct vfs_ref parent;
    char leaf[VFS_NAME_MAX + 1];
    void *made = NULL;
    int32_t rc;

    rc = new_name_parent(path, &parent, leaf, sizeof(leaf));
    if (rc != 0) {
        return rc;
    }
    rc = make_dir_end(&parent, leaf, end, path);
    if (rc == 0) {
        rc = new_name_taken(&parent, leaf, path);
    }
    if (rc != 0) {
        vfs_ref_release(&parent);
        return rc;
    }
    if (vfs_synth_vpath(parent.mnt, parent.node)) {
        vfs_ref_release(&parent);
        return -EROFS;
    }
    if (mnt_is_root(parent.mnt)) {
        rc = cred_access_node((krffs_node_t *)parent.node, CRED_W, 0);
    } else if (parent.mnt->flags & VFS_MS_RDONLY) {
        rc = -EROFS;
    } else {
        rc = cred_access_ref(&parent, CRED_W, 0);
    }
    if (rc == 0 && (type == S_IFCHR || type == S_IFBLK) && !cred_fs_privileged()) {
        rc = -EPERM;
    }
    if (rc == 0) {
        rc = dir_lock(&parent, leaf, RECHECK_WRITE | RECHECK_NEW);
        if (rc == 0) {
            rc = node_make(&parent, leaf, mode, dev, &made);
        }
        vfs_node_unlock(&parent);
        if (rc == 0 && parent.mnt->type->ops->put) {
            parent.mnt->type->ops->put(parent.mnt, made);
        }
    }
    vfs_ref_release(&parent);
    return rc;
}

/**
 * mknod(2): the path, resolved from the working directory, the mode and the
 * device number. The kind in the mode is checked before the path is read
 * (`mknod_kind`); then -EFAULT for a NULL path, the copy's errors, or
 * `do_mknod`'s answer.
 */
long sys_mknod(isr_params_t *params)
{
    const char __user *upath = (const char __user *)SC_ARG(params, 0);
    char *path;
    int end;
    int32_t rc;

    if (!current_proc) {
        return -EFAULT;
    }
    rc = mknod_kind((uint32_t)SC_ARG(params, 1));
    if (rc != 0) {
        return rc;
    }
    if (!upath) {
        return -EFAULT;
    }
    rc = path_from_user_dir(AT_FDCWD, upath, &path, &end);
    if (rc != 0) {
        return rc;
    }
    rc = do_mknod(path, (uint32_t)SC_ARG(params, 1), (uint32_t)SC_ARG(params, 2), end);
    path_buf_put(path);
    return rc;
}

/** mknodat(2): the directory descriptor, the path, the mode and the device
 * number; `sys_mknod` with a relative path resolved from `dirfd`, as openat
 * resolves it. */
long sys_mknodat(isr_params_t *params)
{
    const char __user *upath = (const char __user *)SC_ARG(params, 1);
    char *path;
    int end;
    int32_t rc;

    if (!current_proc) {
        return -EFAULT;
    }
    rc = mknod_kind((uint32_t)SC_ARG(params, 2));
    if (rc != 0) {
        return rc;
    }
    if (!upath) {
        return -EFAULT;
    }
    rc = path_from_user_dir((int)SC_ARG(params, 0), upath, &path, &end);
    if (rc != 0) {
        return rc;
    }
    rc = do_mknod(path, (uint32_t)SC_ARG(params, 2), (uint32_t)SC_ARG(params, 3), end);
    path_buf_put(path);
    return rc;
}

/**
 * Whether the directory `dir` of the KRFFS root on `mnt` is in use: a
 * description has it, an O_PATH descriptor's or one in flight on a socket
 * among them, or it is a process's working directory. A descriptor of the
 * root finds its directory by the node's place, which the allocator gives
 * the next node it makes once the directory is freed, so `rmdir` answers
 * -EBUSY for a directory in use, as a KRFFS volume does (`kv_rmdir`); ext2
 * removes one and keeps its inode for the holders.
 */
static int root_dir_in_use(const struct vfs_mount *mnt, const krffs_node_t *dir)
{
    process_t *pr;

    if (fd_ofd_holders(mnt, dir, NULL) > 0) {
        return 1;
    }
    for (pr = proc_list; pr; pr = pr->next) {
        if (pr->fs && pr->fs->cwd_mnt == mnt && pr->fs->cwd_node == dir) {
            return 1;
        }
    }
    return 0;
}

/**
 * Remove the directory `leaf` from the KRFFS root's directory `parent`: the
 * root's half of `do_rmdir`, run under the root's namespace lock, which the
 * caller took with its walk and keeps until this returns.
 *
 * The answers come in this order: a missing name, -ENOENT; a name that is
 * not a directory, -ENOTDIR; a parent the caller may not write, -EACCES;
 * the sticky rule, -EPERM; a directory in use, -EBUSY (`root_dir_in_use`);
 * and a directory with names in it, -ENOTEMPTY from the core. The use is
 * right only under the lock: an open holds it from its walk until it has
 * published its descriptor, and a chdir sleeps nowhere between the lookup
 * that finds the directory and its record of it.
 */
static int32_t rmdir_root_name(const struct vfs_ref *parent, const char *leaf)
{
    krffs_node_t *node;
    void *found;
    int32_t rc = parent->mnt->type->ops->lookup(parent->mnt, parent->node, leaf, &found);

    if (rc != 0) {
        return rc;
    }
    node = found;
    if (!KRFFS_IS_DIR(node->mode)) {
        return -ENOTDIR;
    }
    rc = cred_access_node((krffs_node_t *)parent->node, CRED_W, 0);
    if (rc == 0) {
        rc = cred_sticky_ok((krffs_node_t *)parent->node, node);
    }
    if (rc == 0 && root_dir_in_use(parent->mnt, node)) {
        rc = -EBUSY;
    }
    return rc != 0 ? rc : vfs_rmdir(parent->mnt, parent->node, leaf);
}

/**
 * Remove the empty directory `path`, an absolute canonical path: the core of
 * rmdir(2) and of unlinkat(2) with AT_REMOVEDIR. Returns 0 or -errno
 * (-ENOENT, -ENOTDIR, -EBUSY, -ENOTEMPTY). The root is -EBUSY, a last name
 * of "." -EINVAL and one of ".." -ENOTEMPTY once the walk to them is made, as
 * on Linux, whatever the directory holds. A mount point and a process's root
 * are -EBUSY, a directory in /dev, /proc or /sys -EROFS but one root made
 * under /dev (`synth_dev_remove`), and any path -ENOENT before the root file
 * system is mounted.
 *
 * The walk keeps the parent's namespace lock, where its type takes one,
 * until the directory is gone: on the KRFFS root the use `rmdir_root_name`
 * finds is right only under that lock. On a type that sets `file_locked`
 * the removal holds the parent's lock and then the victim's
 * (`vfs_child_lock`), under which it looks again for a mount on the victim,
 * which a mount places under the victim's lock too, and for a root there
 * (`dir_recheck`).
 */
static int32_t do_rmdir(const char *pathbuf)
{
    struct vfs_ref parent;
    char leaf[VFS_NAME_MAX + 1];
    int32_t rc;

    if (!vfs_root_mounted()) {
        return -ENOENT;
    }
    if (pathbuf[0] == '/' && pathbuf[1] == '\0') {
        return -EBUSY;
    }
    rc = vfs_resolve(0, pathbuf, VFS_WALK_PARENT | VFS_WALK_LOCK, &parent, leaf, sizeof(leaf));
    if (rc != 0) {
        return rc;
    }
    if (leaf_is_dot(leaf)) {
        rc = leaf[1] == '\0' ? -EINVAL : -ENOTEMPTY;
    } else if (vfs_synth_vpath(parent.mnt, parent.node)) {
        rc = synth_dev_remove(&parent, leaf, 1);
    } else if (vfs_leaf_mount(parent.mnt, parent.node, leaf) ||
               vfs_leaf_root(parent.mnt, parent.node, leaf)) {
        rc = -EBUSY;
    } else if (mnt_is_root(parent.mnt)) {
        rc = rmdir_root_name(&parent, leaf);
    } else {
        struct vfs_ref victim;

        vfs_node_lock(&parent);
        rc = vfs_child_lock(&parent, leaf, &victim);
        if (rc == 0) {
            rc = dir_recheck(&parent, leaf, RECHECK_WRITE | RECHECK_MOUNT | RECHECK_ROOT);
        }
        if (rc == 0) {
            rc = cred_remove_ref(&parent, leaf);
        }
        if (rc == 0) {
            rc = vfs_rmdir(parent.mnt, parent.node, leaf);
        }
        vfs_child_unlock(&victim);
        vfs_node_unlock(&parent);
    }
    vfs_walk_unlock(parent.mnt);
    vfs_ref_release(&parent);
    return rc;
}

/* rmdir(2) takes path. */
long sys_rmdir(isr_params_t *params)
{
    const char __user *upath = (const char __user *)SC_ARG(params, 0);
    char *path;
    int end;
    int32_t rc;

    if (!current_proc || !upath) {
        return -EFAULT;
    }
    rc = path_from_user_dir(AT_FDCWD, upath, &path, &end);
    if (rc != 0) {
        return rc;
    }
    rc = do_rmdir(path);
    path_buf_put(path);
    return rc;
}

/* The unlinkat flag that makes it rmdir. */
#define AT_REMOVEDIR 0x200

/**
 * unlinkat(2) takes dirfd, path and flags. The name is removed as unlink
 * removes it, or as rmdir does with AT_REMOVEDIR; any other flag is -EINVAL
 * before the path is read. A relative path is resolved from `dirfd`.
 */
long sys_unlinkat(isr_params_t *params)
{
    const char __user *upath = (const char __user *)SC_ARG(params, 1);
    int flags = (int)SC_ARG(params, 2);
    char *path;
    int end;
    int32_t rc;

    if (flags & ~AT_REMOVEDIR) {
        return -EINVAL;
    }
    if (!current_proc || !upath) {
        return -EFAULT;
    }
    rc = path_from_user_dir((int)SC_ARG(params, 0), upath, &path, &end);
    if (rc != 0) {
        return rc;
    }
    rc = (flags & AT_REMOVEDIR) ? do_rmdir(path) : unlink_named(path, end);
    path_buf_put(path);
    return rc;
}

/**
 * The refusal RENAME_NOREPLACE adds to a rename: -ENOENT for a source that is
 * not there, as the rename would answer, then -EEXIST for a new name that is,
 * a mount point being there for either, before the EBUSY a mount point is
 * otherwise, as on Linux. The caller holds the rename's locks from this
 * check through the rename (`vfs_rename_lock`), and every change of a name
 * takes its directory's, so no name can appear between the two.
 */
static int32_t rename_noreplace(const struct vfs_ref *opar, const char *oleaf,
                                const struct vfs_ref *npar, const char *nleaf)
{
    int rc = vfs_leaf_mount(opar->mnt, opar->node, oleaf)
                 ? 1
                 : name_present(opar->mnt, opar->node, oleaf);

    if (rc <= 0) {
        return rc == 0 ? -ENOENT : rc;
    }
    if (vfs_leaf_mount(npar->mnt, npar->node, nleaf)) {
        return -EEXIST;
    }
    rc = name_present(npar->mnt, npar->node, nleaf);
    return rc == 1 ? -EEXIST : rc;
}

/**
 * Rename `oleaf` in `opar` to `nleaf` in `npar` on the KRFFS root: the
 * root's half of `do_rename`, run under the root's namespace lock, which the
 * caller takes after both walks and keeps until this returns.
 * RENAME_NOREPLACE in `flags` refuses a new name that exists with -EEXIST,
 * once a missing source has given -ENOENT, as `rename_mounted_names` does.
 *
 * Then a source that is not a directory is -ENOTDIR when either name had a
 * slash after it, `slash`, the order Linux keeps. The sticky rule applies to
 * both names, since taking an entry out of its directory is a removal and so
 * is renaming over one; a caller that checked only the source would leave
 * the attack open while a probe that tries only unlink passes. A directory
 * source is -EPERM, since a directory's children reference its id; a
 * directory destination is -EISDIR; a destination a loop device stands on
 * is -EBUSY.
 *
 * A destination the rename replaces loses its name first when a descriptor
 * holds it, so the holder keeps reading the file it opened, and is freed by
 * the rename when none does. That count comes after every refusal, since a
 * rename that then failed would have taken the name of the file it refused
 * to touch, and it is right only under the lock, which an open holds until
 * it has published its descriptor.
 */
static int32_t rename_root_names(const struct vfs_ref *opar, const char *oleaf,
                                 const struct vfs_ref *npar, const char *nleaf, int flags,
                                 int slash)
{
    krffs_node_t *node;
    krffs_node_t *existing = NULL;
    void *found;
    int32_t rc = opar->mnt->type->ops->lookup(opar->mnt, opar->node, oleaf, &found);

    if (rc != 0) {
        return rc;
    }
    node = found;
    rc = (flags & RENAME_NOREPLACE) ? rename_noreplace(opar, oleaf, npar, nleaf) : 0;
    if (rc != 0) {
        return rc;
    }
    if (slash && node != krffs_fs->node && !KRFFS_IS_DIR(node->mode)) {
        return -ENOTDIR;
    }
    rc = cred_sticky_ok((krffs_node_t *)opar->node, node);
    if (rc != 0) {
        return rc;
    }
    if (node == krffs_fs->node || KRFFS_IS_DIR(node->mode)) {
        return -EPERM;
    }
    if ((opar->mnt->flags | vfs_mount_base(opar->mnt)->flags) & VFS_MS_RDONLY) {
        return -EROFS;
    }
    if (flags & RENAME_NOREPLACE) {
        return vfs_rename(opar->mnt, opar->node, oleaf, npar->node, nleaf);
    }
    if (npar->mnt->type->ops->lookup(npar->mnt, npar->node, nleaf, &found) == 0) {
        existing = found;
    }
    if (existing && existing != node) {
        rc = cred_sticky_ok((krffs_node_t *)npar->node, existing);
        if (rc != 0) {
            return rc;
        }
        if (existing == krffs_fs->node || KRFFS_IS_DIR(existing->mode)) {
            return -EISDIR;
        }
        if (loop_backs(npar->mnt, existing)) {
            return -EBUSY;
        }
        if (fd_node_holders(existing, NULL) > 0) {
            krffs_orphan_node(existing);
        }
    }
    return vfs_rename(opar->mnt, opar->node, oleaf, npar->node, nleaf);
}

/**
 * Rename `oleaf` in `opar` to `nleaf` in `npar` on a mounted type: the
 * other half of `do_rename`, which holds the rename's locks
 * (`vfs_rename_lock`) from the first lookup through the rename: the mount's
 * namespace lock, or on a type that sets `file_locked` the mount's rename
 * lock, both directories' and the nodes of both names, under which the
 * write permission on both directories and the mounts on both names are
 * checked again (`dir_recheck`), since the walks held nothing.
 * RENAME_NOREPLACE in `flags` refuses a new name that exists with -EEXIST;
 * a source that is not a directory is -ENOTDIR when either name had a slash
 * after it, `slash`; then the removal rules of `cred_remove_ref` for both
 * names, and the type's own answers.
 */
static int32_t rename_mounted_names(const struct vfs_ref *opar, const char *oleaf,
                                    const struct vfs_ref *npar, const char *nleaf, int flags,
                                    int slash)
{
    struct vfs_rename_hold h;
    int32_t rc;

    rc = vfs_rename_lock(opar, oleaf, npar, nleaf, &h);
    if (rc != 0) {
        return rc;
    }
    rc = dir_recheck(opar, oleaf, RECHECK_WRITE | RECHECK_MOUNT);
    if (rc == 0) {
        rc = dir_recheck(npar, nleaf, RECHECK_WRITE | RECHECK_MOUNT);
    }
    if (rc == 0 && (flags & RENAME_NOREPLACE)) {
        rc = rename_noreplace(opar, oleaf, npar, nleaf);
    }
    if (rc == 0 && slash) {
        rc = leaf_dir_check(opar, oleaf);
    }
    if (rc == 0) {
        rc = cred_remove_ref(opar, oleaf);
    }
    if (rc == 0) {
        rc = cred_remove_ref(npar, nleaf);
    }
    if (rc == 0) {
        rc = vfs_rename(opar->mnt, opar->node, oleaf, npar->node, nleaf);
    }
    vfs_rename_unlock(&h);
    return rc;
}

/** RENAME_NOREPLACE when a name of the rename is a mount point: -EEXIST when
 * the new name is there, -EBUSY otherwise, or the locks' error. */
static int32_t rename_on_mount(const struct vfs_ref *opar, const char *oleaf,
                               const struct vfs_ref *npar, const char *nleaf)
{
    struct vfs_rename_hold h;
    int32_t rc;

    rc = vfs_rename_lock(opar, oleaf, npar, nleaf, &h);
    if (rc == 0) {
        rc = rename_noreplace(opar, oleaf, npar, nleaf);
        vfs_rename_unlock(&h);
    }
    return rc == 0 ? -EBUSY : rc;
}

/**
 * Rename `old_buf` to `new_buf`, both absolute canonical paths: the core of
 * rename(2), renameat(2) and renameat2(2), whose RENAME_NOREPLACE in `flags`
 * refuses a new name that exists with -EEXIST, decided with the rename under
 * one namespace lock. `slash` says that either path had a slash after its
 * last name. Returns 0 or -errno.
 *
 * Either path naming the root is -EBUSY, or -EEXIST for the new path under
 * RENAME_NOREPLACE, before either path is walked, where Linux walks both
 * first: the path copy canonicalizes the root to "/", which has no last name
 * for a walk to the parent to give. The walks give -ENOENT for a missing
 * parent and -ENOTDIR for a file used as a directory, and two mounts are
 * -EXDEV, after which a program copies and unlinks. A last name of "." or
 * ".." in either path, which the walk to its parent returns as the name, is
 * then -EBUSY, and in the new path -EEXIST under RENAME_NOREPLACE, as on
 * Linux, which checks these before it asks for write access: before the
 * write bits, and before the -EROFS of /dev, /proc and /sys, where nothing
 * is renamed.
 *
 * A rename writes both directories, the entry leaving one and landing in the
 * other, so both write bits are checked before either name is looked up, as
 * unlink orders it: a caller without them sees -EACCES where Linux would
 * report the lookups' -ENOENT or -EEXIST. A mount point at either name is
 * -EBUSY, after the -ENOENT and -EEXIST of RENAME_NOREPLACE.
 *
 * On the KRFFS root every other refusal is made here and in
 * `rename_root_names`, since `krffs_rename` answers each with -1, which
 * `kfs_rename` reports as -EIO. Of the two directory cases POSIX separates,
 * a directory source is -EPERM, since a directory's children refer to its
 * id and a move would orphan them, and a directory destination under a
 * plain source is -EISDIR, the case that errno is for. A mounted KRFFS
 * volume gives the same answers (`kv_rename`), and ext2 moves directories.
 * The root, refused before the walks, never reaches the identity tests of
 * `rename_root_names`; they keep a replaced destination that is the root
 * from being freed, which would take the file system with it, whatever the
 * path copy does.
 *
 * Both names are walked without the namespace lock, which is taken for the
 * lookups and the rename that follow, or on a type that sets `file_locked`
 * the rename's locks of `vfs_rename_lock`. A walk takes the lock of each
 * mount it looks in, as ext2's lookup does, so a rename that walked its new
 * name holding the lock of the old one would hold one mount's lock while it
 * waited for another's, and two renames, or a rename and a read through a
 * loop device, could wait for each other. On the KRFFS root the holder count of
 * the replaced file is right only under the lock, which an open holds until
 * it has published its descriptor.
 */
static int32_t do_rename(const char *old_buf, const char *new_buf, int flags, int slash)
{
    char oleaf[VFS_NAME_MAX + 1];
    char nleaf[VFS_NAME_MAX + 1];
    const char *old_name;
    const char *new_name;
    struct vfs_ref opar;
    struct vfs_ref npar;
    int32_t rc;

    old_name = old_buf;
    if (old_name[0] == '/') {
        old_name++;
    }
    new_name = new_buf;
    if (new_name[0] == '/') {
        new_name++;
    }

    if (old_name[0] == '\0' || new_name[0] == '\0') {
        return (old_name[0] != '\0' && (flags & RENAME_NOREPLACE)) ? -EEXIST : -EBUSY;
    }

    rc = vfs_resolve(0, old_buf, VFS_WALK_PARENT, &opar, oleaf, sizeof(oleaf));
    if (rc != 0) {
        return rc;
    }
    rc = vfs_resolve(0, new_buf, VFS_WALK_PARENT, &npar, nleaf, sizeof(nleaf));
    if (rc != 0) {
        vfs_ref_release(&opar);
        return rc;
    }
    /* rename is within one mount; across two it is the common layer's
     * EXDEV, and a program falls back to copy and unlink. */
    if (opar.mnt != npar.mnt) {
        rc = -EXDEV;
        goto out;
    }
    if (leaf_is_dot(oleaf) || leaf_is_dot(nleaf)) {
        rc = (!leaf_is_dot(oleaf) && (flags & RENAME_NOREPLACE)) ? -EEXIST : -EBUSY;
        goto out;
    }
    rc = mnt_is_root(opar.mnt) ? cred_access_node((krffs_node_t *)opar.node, CRED_W, 0)
                               : cred_access_ref(&opar, CRED_W, 0);
    if (rc == 0) {
        rc = mnt_is_root(npar.mnt) ? cred_access_node((krffs_node_t *)npar.node, CRED_W, 0)
                                   : cred_access_ref(&npar, CRED_W, 0);
    }
    if (rc == 0 && vfs_synth_vpath(opar.mnt, opar.node)) {
        rc = synth_dev_rename(&opar, oleaf, &npar, nleaf, new_buf,
                              (uint32_t)flags | (slash ? RENAME_SLASH : 0u));
        goto out;
    }
    if (rc != 0) {
        goto out;
    }
    if (vfs_leaf_mount(opar.mnt, opar.node, oleaf) || vfs_leaf_mount(npar.mnt, npar.node, nleaf)) {
        rc = (flags & RENAME_NOREPLACE) ? rename_on_mount(&opar, oleaf, &npar, nleaf) : -EBUSY;
        goto out;
    }
    vfs_tree_changed();
    if (!mnt_is_root(vfs_mount_base(opar.mnt))) {
        rc = rename_mounted_names(&opar, oleaf, &npar, nleaf, flags, slash);
    } else {
        vfs_ns_lock(opar.mnt);
        rc = rename_root_names(&opar, oleaf, &npar, nleaf, flags, slash);
        vfs_ns_unlock(opar.mnt);
    }
    vfs_tree_changed();
out:
    vfs_ref_release(&npar);
    vfs_ref_release(&opar);
    return rc;
}

/**
 * rename(2), renameat(2) and renameat2(2) after their arguments: both paths
 * are copied, each relative one joined to its own directory, before either
 * is resolved, a last "." kept as the name the walk to the parent returns
 * (`path_put_dot`). Returns 0 or a negative errno: the copies' or
 * `do_rename`'s.
 */
static int32_t rename_from_user(int olddirfd, const char __user *uold, int newdirfd,
                                const char __user *unew, int flags)
{
    char *oldpath = NULL;
    char *newpath = NULL;
    int oldend = 0;
    int newend = 0;
    int32_t rc = path_from_user_dir(olddirfd, uold, &oldpath, &oldend);

    if (rc == 0) {
        rc = path_from_user_dir(newdirfd, unew, &newpath, &newend);
    }
    if (rc == 0) {
        rc = do_rename(oldpath, newpath, flags,
                       oldend == PATH_END_SLASH || newend == PATH_END_SLASH);
    }
    path_buf_put(newpath);
    path_buf_put(oldpath);
    return rc;
}

/** rename(2): the old and the new path, each resolved from the working
 * directory; -EFAULT for a NULL path, or `rename_from_user`'s answer. */
long sys_rename(isr_params_t *params)
{
    const char __user *old_path = (const char __user *)SC_ARG(params, 0);
    const char __user *new_path = (const char __user *)SC_ARG(params, 1);

    if (!current_proc || !old_path || !new_path) {
        return -EFAULT;
    }
    return rename_from_user(AT_FDCWD, old_path, AT_FDCWD, new_path, 0);
}

/**
 * renameat(2): the old directory descriptor and path and the new ones; each
 * relative path is resolved from its own descriptor. -EFAULT for a NULL
 * path, or `rename_from_user`'s answer.
 */
long sys_renameat(isr_params_t *params)
{
    const char __user *old_path = (const char __user *)SC_ARG(params, 1);
    const char __user *new_path = (const char __user *)SC_ARG(params, 3);

    if (!current_proc || !old_path || !new_path) {
        return -EFAULT;
    }
    return rename_from_user((int)SC_ARG(params, 0), old_path, (int)SC_ARG(params, 2), new_path, 0);
}

/**
 * renameat2(2): renameat's arguments and the flags. RENAME_NOREPLACE
 * refuses a new name that exists with -EEXIST; RENAME_EXCHANGE,
 * RENAME_WHITEOUT and any other flag are -EINVAL before a path is read, as a
 * Linux file system without them answers them later.
 */
long sys_renameat2(isr_params_t *params)
{
    const char __user *old_path = (const char __user *)SC_ARG(params, 1);
    const char __user *new_path = (const char __user *)SC_ARG(params, 3);
    int flags = (int)SC_ARG(params, 4);

    if (flags & ~RENAME_NOREPLACE) {
        return -EINVAL;
    }
    if (!current_proc || !old_path || !new_path) {
        return -EFAULT;
    }
    return rename_from_user((int)SC_ARG(params, 0), old_path, (int)SC_ARG(params, 2), new_path,
                            flags);
}

/* utimensat's tv_nsec for a time set to now, and for a time left alone. */
#define UTIME_NOW ((1L << 30) - 1L)

#define UTIME_OMIT ((1L << 30) - 2L)

void get_current_time(int64_t *seconds, int64_t *nanoseconds)
{
    rtc_realtime(seconds, nanoseconds);
}

/* A time utimensat is given: seconds of 64 bits, which i686's
 * utimensat_time64 fills past 2038, and nanoseconds, UTIME_NOW or
 * UTIME_OMIT. */
struct utime_spec {
    int64_t tv_sec;
    long tv_nsec;
};

/** Whether `nsec` is a time's nanoseconds, UTIME_NOW or UTIME_OMIT. */
static int utime_nsec_valid(long nsec)
{
    return nsec == UTIME_NOW || nsec == UTIME_OMIT || (nsec >= 0 && nsec <= 999999999);
}

/**
 * Set the access and modification times of the file at (`mnt`, `node`), for
 * utimensat by path and by descriptor. `times` is the pair [atime, mtime];
 * NULL sets both to now, a tv_nsec of UTIME_NOW sets that one to now and
 * UTIME_OMIT leaves it, and any change advances ctime.
 *
 * The POSIX permission split: setting both to now is the owner's, or anyone's
 * with write permission, or root's; an explicit time is the owner's or root's
 * alone. A tv_nsec out of range is -EINVAL, as on Linux. A synthetic file has
 * no stored times and is -EROFS; the KRFFS root's times are not changed and
 * the call succeeds. On another type the read, the check and the store are
 * one step under the file's lock (`vfs_node_lock`), as for chmod and chown.
 */
static int32_t utimes_node(struct vfs_mount *mnt, void *node, const struct utime_spec *times)
{
    krffs_node_t *kn = node;
    int64_t now_sec, now_nsec;
    int now_both =
        times == NULL || (times[0].tv_nsec == UTIME_NOW && times[1].tv_nsec == UTIME_NOW);
    int32_t rc;

    if (times && (!utime_nsec_valid(times[0].tv_nsec) || !utime_nsec_valid(times[1].tv_nsec))) {
        return -EINVAL;
    }
    if (vfs_synth_vpath(mnt, node)) {
        return -EROFS;
    }
    get_current_time(&now_sec, &now_nsec);
    if (!mnt_is_root(mnt)) {
        struct vfs_ref ref;
        struct vfs_stat vst;
        int what = 0;

        ref.mnt = mnt;
        ref.node = node;
        vfs_node_lock(&ref);
        rc = vfs_getattr(mnt, node, &vst);
        if (rc == 0 && !cred_fs_privileged() && proc_fsuid(current_proc) != vst.uid) {
            rc = now_both ? cred_access_ref(&ref, CRED_W, 0) : -EPERM;
        }
        if (rc == 0 && (times == NULL || times[0].tv_nsec != UTIME_OMIT)) {
            int now = times == NULL || times[0].tv_nsec == UTIME_NOW;

            vst.atime_sec = now ? now_sec : times[0].tv_sec;
            vst.atime_nsec = now ? now_nsec : times[0].tv_nsec;
            what |= VFS_SET_ATIME;
        }
        if (rc == 0 && (times == NULL || times[1].tv_nsec != UTIME_OMIT)) {
            int now = times == NULL || times[1].tv_nsec == UTIME_NOW;

            vst.mtime_sec = now ? now_sec : times[1].tv_sec;
            vst.mtime_nsec = now ? now_nsec : times[1].tv_nsec;
            what |= VFS_SET_MTIME;
        }
        if (rc == 0) {
            rc = vfs_setattr(mnt, node, what, &vst);
        }
        vfs_node_unlock(&ref);
        return rc;
    }
    if (kn == krffs_fs->node) {
        return 0;
    }
    if (now_both) {
        if (!cred_fs_privileged() && proc_fsuid(current_proc) != kn->uid &&
            cred_access_node(kn, CRED_W, 0) != 0) {
            return -EACCES;
        }
    } else if (!cred_fs_privileged() && proc_fsuid(current_proc) != kn->uid) {
        return -EPERM;
    }
    if (times == NULL || times[0].tv_nsec != UTIME_OMIT) {
        int now = times == NULL || times[0].tv_nsec == UTIME_NOW;

        kn->access_time.seconds = now ? now_sec : times[0].tv_sec;
        kn->access_time.nanoseconds = now ? now_nsec : times[0].tv_nsec;
    }
    if (times == NULL || times[1].tv_nsec != UTIME_OMIT) {
        int now = times == NULL || times[1].tv_nsec == UTIME_NOW;

        kn->modification_time.seconds = now ? now_sec : times[1].tv_sec;
        kn->modification_time.nanoseconds = now ? now_nsec : times[1].tv_nsec;
    }
    kn->attribute_change_time.seconds = now_sec;
    kn->attribute_change_time.nanoseconds = now_nsec;
    return 0;
}

/**
 * The inotify events of a change of the times `times`, which `utimes_node`
 * takes: both times are IN_ATTRIB, the access time alone IN_ACCESS and the
 * modification time alone IN_MODIFY, as `vfs_setattr_events` gives them. The
 * KRFFS root, whose changes do not pass through `vfs_setattr`, and the
 * directory's watches are told them by the callers.
 */
static uint32_t utimes_events(const struct utime_spec *times)
{
    int what = 0;

    if (times == NULL || times[0].tv_nsec != UTIME_OMIT) {
        what |= VFS_SET_ATIME;
    }
    if (times == NULL || times[1].tv_nsec != UTIME_OMIT) {
        what |= VFS_SET_MTIME;
    }
    return vfs_setattr_events(what);
}

/**
 * utimensat on the file descriptor `fd` refers to: a NULL path, which is GNU
 * libc's futimens, and an empty one with AT_EMPTY_PATH, which `empty` says.
 *
 * A file or a directory of a file system has its times set by the rules of
 * `utimes_node`, and one of the KRFFS boot root must still be the node it
 * was, or have lost its name to an unlink since. A pipe, a socket, a
 * terminal, a device or a synthetic directory has no times stored for it and
 * is -EROFS, as the path to a synthetic one answers; Linux sets the times of
 * such an inode. An O_PATH descriptor names its file for the empty path and
 * is -EBADF to the NULL one, as on Linux.
 *
 * The description's node is read under the mount's namespace lock, which
 * `utimes_node` takes again: a KRFFS write that grows the file moves it
 * under that lock and repoints the description, so a node read before the
 * wait for the lock could be the storage the file left.
 */
static int32_t utimes_fd(int fd, const struct utime_spec *times, int empty)
{
    fd_entry_t *entry = fd_get(current_proc->fds, fd);
    struct vfs_ref file;
    struct vfs_mount *mnt;
    int32_t rc;

    if (!entry || (!empty && (entry->flags & FD_FLAG_PATH))) {
        return -EBADF;
    }
    if (entry->type != FD_TYPE_FILE && entry->type != FD_TYPE_DIR) {
        return -EROFS;
    }
    if (!entry->ofd || !entry->ofd->vmnt) {
        return -EBADF;
    }
    mnt = entry->ofd->vmnt;
    file.mnt = mnt;
    file.node = entry->ofd->vnode;
    vfs_node_lock(&file);
    if (entry->node && fd_node_check(entry) != 0) {
        rc = -EBADF;
    } else {
        rc = utimes_node(mnt, entry->ofd->vnode, times);
    }
    vfs_node_unlock(&file);
    if (rc == 0) {
        attr_told_desc(entry->ofd, utimes_events(times), mnt_is_root(mnt));
    }
    return rc;
}

/**
 * utimensat on the file the absolute canonical path `path` names, a final
 * symbolic link's own times under AT_SYMLINK_NOFOLLOW in `flags`, by the
 * rules of `utimes_node`; a path that named a directory by its end, `dir`,
 * follows a final link either way and must reach a directory
 * (`resolve_named`). A type that serialises its uses keeps the final mount's
 * namespace lock from the resolution to the store, so a KRFFS write cannot
 * move the file between the two.
 */
static int32_t utimes_path(const char *path, int flags, int dir, const struct utime_spec *times)
{
    struct vfs_ref ref;
    int32_t rc;

    rc = resolve_named(
        path, ((flags & AT_SYMLINK_NOFOLLOW) ? VFS_WALK_NOFOLLOW : 0) | VFS_WALK_LOCK, dir, &ref);
    if (rc != 0) {
        return rc;
    }
    rc = utimes_node(ref.mnt, ref.node, times);
    vfs_walk_unlock(ref.mnt);
    if (rc == 0) {
        attr_told(path, ref.mnt, ref.node, utimes_events(times), mnt_is_root(ref.mnt));
    }
    vfs_ref_release(&ref);
    return rc;
}

/**
 * Read the pair of times utimensat is given at `u` into `t`: two timespecs of
 * longs, 32 bits on i686 and 64 on x86-64, or with `wide` of 64-bit fields,
 * utimensat_time64's on i686. Of a wide tv_nsec only the low 32 bits count,
 * as on Linux i386, since GNU libc leaves the high half as padding. 0 or
 * -EFAULT.
 */
static int32_t utimes_get(const void __user *u, int wide, struct utime_spec *t)
{
    if (wide) {
        int64_t v[4];

        if (copy_from_user(v, u, sizeof(v)) != 0) {
            return -EFAULT;
        }
        t[0].tv_sec = v[0];
        t[0].tv_nsec = (int32_t)v[1];
        t[1].tv_sec = v[2];
        t[1].tv_nsec = (int32_t)v[3];
    } else {
        struct kernel_timespec v[2];

        if (copy_from_user(v, u, sizeof(v)) != 0) {
            return -EFAULT;
        }
        t[0].tv_sec = v[0].tv_sec;
        t[0].tv_nsec = v[0].tv_nsec;
        t[1].tv_sec = v[1].tv_sec;
        t[1].tv_nsec = v[1].tv_nsec;
    }
    return 0;
}

/**
 * The body of utimensat, and of utimensat_time64 when `wide`, whose times
 * `utimes_get` reads.
 *
 * Its arguments are dirfd, path, times and flags. `times` is the pair [atime,
 * mtime] or NULL, as `utimes_node` takes it, and a pair of UTIME_OMIT changes
 * nothing and returns 0 before anything else is looked at, as on Linux. A NULL
 * path names the file `dirfd` refers to, with no flags (-EINVAL otherwise);
 * with AT_FDCWD it is -EFAULT. Otherwise the path resolves as openat's does,
 * AT_SYMLINK_NOFOLLOW sets a final symbolic link's own times, and with
 * AT_EMPTY_PATH an empty path names the file `dirfd` refers to, or the working
 * directory for AT_FDCWD; `utimes_path` sets the times. Returns 0 or -errno.
 */
static long utimensat_body(isr_params_t *params, int wide)
{
    int dirfd = (int)SC_ARG(params, 0);
    const char __user *user_path = (const char __user *)SC_ARG(params, 1);
    const void __user *user_times = (const void __user *)SC_ARG(params, 2);
    int flags = (int)SC_ARG(params, 3);
    struct utime_spec ktimes[2];
    const struct utime_spec *times = NULL;
    char *pathbuf;
    int empty = 0;
    int dir = 0;
    int32_t rc;

    if (!current_proc) {
        return -EFAULT;
    }
    if (user_times) {
        if (utimes_get(user_times, wide, ktimes) != 0) {
            return -EFAULT;
        }
        if (ktimes[0].tv_nsec == UTIME_OMIT && ktimes[1].tv_nsec == UTIME_OMIT) {
            return 0;
        }
        times = ktimes;
    }
    if (!user_path && dirfd != AT_FDCWD) {
        return flags != 0 ? -EINVAL : utimes_fd(dirfd, times, 0);
    }
    if (flags & ~(AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH)) {
        return -EINVAL;
    }
    if (!user_path) {
        return -EFAULT;
    }
    if (flags & AT_EMPTY_PATH) {
        empty = user_path_empty(user_path);
        if (empty < 0) {
            return empty;
        }
    }
    if (empty && dirfd != AT_FDCWD) {
        return utimes_fd(dirfd, times, 1);
    }
    if (empty) {
        pathbuf = path_buf_get();
        if (!pathbuf) {
            return -ENOMEM;
        }
        k_strncpy(pathbuf, proc_cwd(current_proc), VFS_PATH_MAX);
    } else {
        rc = path_from_user_dir(dirfd, user_path, &pathbuf, &dir);
        if (rc != 0) {
            return rc;
        }
    }
    rc = utimes_path(pathbuf, flags, dir, times);
    path_buf_put(pathbuf);
    return rc;
}

/** Update a file's access and modification timestamps (utimensat(2)), with
 * the timespec of longs; `utimensat_body` has the contract. */
long sys_utimensat(isr_params_t *params)
{
    return utimensat_body(params, 0);
}

/** utimensat_time64: utimensat with the 64-bit timespec, i686's call for a
 * time past 2038. */
long sys_utimensat_time64(isr_params_t *params)
{
    return utimensat_body(params, 1);
}

/* A record of getdents64(2), Linux's struct linux_dirent64. */
struct linux_dirent64 {
    uint64_t d_ino;    /* inode number */
    int64_t d_off;     /* the directory's offset after this record */
    uint16_t d_reclen; /* bytes in the record, a multiple of 8 */
    uint8_t d_type;    /* a DT_ value, DT_UNKNOWN for a type not given */
    char d_name[1];    /* the name and its NUL, which run past the struct */
};

/* Directory entry types */
#define DT_REG 8 /* Regular file */

#define DT_DIR 4 /* Directory */

#ifndef DT_CHR

#define DT_CHR 2

#endif

#ifndef DT_LNK

#define DT_LNK 10

#endif

/** Write `v` in decimal into `out`, which has room for 11 bytes, with its
 * terminator. */
static void synth_u32_str(char *out, uint32_t v)
{
    char tmp[12];
    int n = 0;
    int i = 0;

    if (v == 0) {
        out[0] = '0';
        out[1] = '\0';
        return;
    }
    while (v) {
        tmp[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    }
    while (n) {
        out[i++] = tmp[--n];
    }
    out[i] = '\0';
}

/**
 * The entry at position `*at`, "." and ".." counted off, of a process's
 * directory or of its `task`, `fd`, `fdinfo` or `ns` directory, from the
 * directory descriptor's record `sd`: fills name/ino/dtype and returns 1, or
 * 0 past the end or once the process, or the thread a thread's directory is
 * of, is gone, which leaves the directory with "." and ".." alone. A thread's
 * directory has the files of the thread with no `task` among them, and its
 * fd directory the thread's descriptors, and `children` takes the place of
 * `task`, the last entry, in a thread's directory. `task` lists the process's
 * threads, the leader first, without a thread that has ended and is not yet
 * reaped unless it is the leader.
 *
 * The n-th entry is at position n, but in `fd` and `fdinfo` a descriptor is
 * at its number: the entry is the lowest open descriptor at `*at` or above,
 * and `*at` is set to its number. So a descriptor closed between two reads of
 * the directory moves no other, as a program that closes each descriptor it
 * reads before an exec needs.
 */
static int synth_proc_dirent(const struct synth_dir *sd, int *at, char *name, uint64_t *ino,
                             uint8_t *dtype)
{
    static const struct {
        const char *name;
        uint8_t type;
    } files[] = {{"stat", DT_REG},    {"cmdline", DT_REG},   {"fd", DT_DIR},
                 {"maps", DT_REG},    {"smaps", DT_REG},     {"status", DT_REG},
                 {"statm", DT_REG},   {"auxv", DT_REG},      {"environ", DT_REG},
                 {"comm", DT_REG},    {"mem", DT_REG},       {"oom_score_adj", DT_REG},
                 {"mounts", DT_REG},  {"mountinfo", DT_REG}, {"limits", DT_REG},
                 {"uid_map", DT_REG}, {"gid_map", DT_REG},   {"setgroups", DT_REG},
                 {"ns", DT_DIR},      {"fdinfo", DT_DIR},    {"exe", DT_LNK},
                 {"cwd", DT_LNK},     {"root", DT_LNK},      {"numa_maps", DT_REG},
                 {"syscall", DT_REG}, {"wchan", DT_REG},     {"schedstat", DT_REG},
                 {"task", DT_DIR}};
    struct vpath vp;
    process_t *pp;
    int n = *at;
    int id, i, c = 0;

    synth_dir_vpath(sd, &vp);
    id = procfs_resolve_id(&vp);
    pp = id < 0 ? NULL : get_process((uint32_t)id);
    if (!pp) {
        return 0;
    }
    if (sd->kind == VNODE_PROC_PID) {
        int last = (int)(sizeof(files) / sizeof(files[0])) - 1;
        const char *entry;

        if (n > last) {
            return 0;
        }
        entry = files[n].name;
        *dtype = files[n].type;
        if (n == last && sd->tid != 0) {
            entry = "children";
            *dtype = DT_REG;
        }
        for (i = 0; entry[i]; i++) {
            name[i] = entry[i];
        }
        name[i] = '\0';
        *ino = procfs_ino_pid(id);
        return 1;
    }
    if (sd->kind == VNODE_PROC_NS_DIR) {
        const char *kind = synth_ns_name(n);

        if (!kind) {
            return 0;
        }
        for (i = 0; kind[i]; i++) {
            name[i] = kind[i];
        }
        name[i] = '\0';
        *ino = procfs_ino_pid(id);
        *dtype = DT_LNK;
        return 1;
    }
    if (sd->kind == VNODE_PROC_TASK_DIR) {
        process_t *t;

        for (t = proc_list; t; t = t->next) {
            if (t->tg != pp->tg || t->state == PROC_UNUSED ||
                (t->state == PROC_ZOMBIE && t != pp->tg->leader)) {
                continue;
            }
            if (c++ == n) {
                synth_u32_str(name, pid_seen(t->pid));
                *ino = procfs_ino_pid((int)t->pid);
                *dtype = DT_DIR;
                return 1;
            }
        }
        return 0;
    }
    i = fd_next_open(pp->fds, n);
    if (i < 0) {
        return 0;
    }
    *at = i;
    synth_u32_str(name, (uint32_t)i);
    *ino = procfs_ino_pid(id);
    *dtype = sd->kind == VNODE_PROC_FDINFO_DIR ? DT_REG : DT_LNK;
    return 1;
}

/**
 * The `n`th entry of /sys/class/net or of an interface's directory under it,
 * `node`, whose interface is `key`: an interface's directory each in the
 * first; its attribute files, its `statistics` and its `device` link in the
 * second; and the counters in `statistics`. 1, or 0 past the last.
 */
static int synth_sys_net_dirent(int node, int key, int n, char *name, uint64_t *ino, uint8_t *dtype)
{
    const char *attr;
    int i;

    if (node == SYSFS_CLASS_NET) {
        struct netdev *nd = n < netdev_seen_count() ? netdev_at(n) : NULL;

        if (!nd) {
            return 0;
        }
        sysfs_net_name(nd->index, name, 32);
        *ino = sysfs_ino(SYSFS_NET_IF, nd->index, 0);
        *dtype = DT_DIR;
        return 1;
    }
    for (i = 0; (attr = sysfs_attr(node, i)) != NULL; i++) {
        if (i == n) {
            k_strncpy(name, attr, 31);
            name[31] = '\0';
            *ino = sysfs_ino(node, key, 1 + i);
            *dtype = DT_REG;
            return 1;
        }
    }
    if (node == SYSFS_NET_IF && n == i) {
        k_strncpy(name, "statistics", 31);
        *ino = sysfs_ino(SYSFS_NET_STATS, key, 0);
        *dtype = DT_DIR;
        return 1;
    }
    if (node == SYSFS_NET_IF && sysfs_net_wiphy(key) >= 0) {
        int phy = sysfs_net_wiphy(key);
        if (n == i + 1 || n == i + 2) {
            k_strncpy(name, n == i + 1 ? "device" : "phy80211", 31);
            *ino = sysfs_ino(n == i + 1 ? SYSFS_WLAN_DEVICE : SYSFS_WLAN_PHY, phy, 0);
            *dtype = n == i + 1 ? DT_DIR : DT_LNK;
            return 1;
        }
        return 0;
    }
    if (node == SYSFS_NET_IF && n == i + 1 && sysfs_net_pci_key(key) >= 0) {
        k_strncpy(name, "device", 31);
        *ino = sysfs_ino(SYSFS_PCI_DEVICE, sysfs_net_pci_key(key), 2);
        *dtype = DT_LNK;
        return 1;
    }
    return 0;
}

/** The `n`th disk node of devfs's table that /sys/block lists: a partition
 * in the directory of the disk node `key`, a whole disk for -1, and any for
 * -2; its index, or -1 past the last. */
static int sysfs_block_nth(int key, int n)
{
    struct devfs_disk_row row;
    int i, got;

    for (i = 0; (got = devfs_disk_row(i, &row)) >= 0; i++) {
        if (got == 1 &&
            (key == -2 || (key < 0 ? row.partno == 0 : row.partno != 0 && row.whole == key)) &&
            n-- == 0) {
            return i;
        }
    }
    return -1;
}

/** Write MAJOR:MINOR into `out`, which has room for it. */
static void synth_put_devno(char *out, uint32_t major, uint32_t minor)
{
    char digits[12];
    uint32_t v[2];
    int pos = 0, k, d;

    v[0] = major;
    v[1] = minor;
    for (k = 0; k < 2; k++) {
        d = 0;
        do {
            digits[d++] = (char)('0' + v[k] % 10u);
            v[k] /= 10u;
        } while (v[k] != 0);
        while (d > 0) {
            out[pos++] = digits[--d];
        }
        if (k == 0) {
            out[pos++] = ':';
        }
    }
    out[pos] = '\0';
}

/**
 * The `n`th entry of a directory of disks: /sys/block, a directory for each
 * disk; /sys/dev/block, a link named MAJOR:MINOR for each disk node; a
 * disk's own, its attributes, `queue`, `device`, a bound loop device's
 * `loop` and its partitions' directories; and the others, their attributes.
 * 1 with the entry's name, inode and type, or 0 past the last.
 */
static int synth_sys_block_dirent(int node, int key, int n, char *name, uint64_t *ino,
                                  uint8_t *dtype)
{
    struct devfs_disk_row row;
    const char *attr = SYSFS_IS_BLOCK(node) ? sysfs_attr(node, n) : NULL;
    int i, count = 0;

    if (attr) {
        k_strncpy(name, attr, 31);
        name[31] = '\0';
        *ino = sysfs_ino(node, key, 1 + n);
        *dtype = DT_REG;
        return 1;
    }
    while (SYSFS_IS_BLOCK(node) && sysfs_attr(node, count)) {
        count++;
    }
    n -= count;
    if (node == SYSFS_BLOCK_DISK && n == 2 && sysfs_block_loop(key) >= 0) {
        k_strncpy(name, "loop", 31);
        *ino = sysfs_ino(SYSFS_BLOCK_LOOP, key, 0);
        *dtype = DT_DIR;
        return 1;
    }
    if (node == SYSFS_BLOCK_DISK && (n == 0 || n == 1)) {
        k_strncpy(name, n == 0 ? "queue" : "device", 31);
        *ino = sysfs_ino(n == 0 ? SYSFS_BLOCK_QUEUE : SYSFS_BLOCK_DEV, key, 0);
        *dtype = DT_DIR;
        return 1;
    }
    if (node != SYSFS_BLOCK && node != SYSFS_DEV_BLOCK && node != SYSFS_BLOCK_DISK) {
        return 0;
    }
    i = sysfs_block_nth(node == SYSFS_BLOCK        ? -1
                        : node == SYSFS_BLOCK_DISK ? key
                                                   : -2,
                        node == SYSFS_BLOCK_DISK ? n - 2 - (sysfs_block_loop(key) >= 0) : n);
    if (i < 0 || devfs_disk_row(i, &row) != 1) {
        return 0;
    }
    if (node == SYSFS_DEV_BLOCK) {
        synth_put_devno(name, row.major, row.minor);
        *ino = sysfs_ino(row.partno ? SYSFS_BLOCK_PART : SYSFS_BLOCK_DISK, i, 0xFF);
        *dtype = DT_LNK;
        return 1;
    }
    k_strncpy(name, row.name, 31);
    name[31] = '\0';
    *ino = sysfs_ino(row.partno ? SYSFS_BLOCK_PART : SYSFS_BLOCK_DISK, i, 0);
    *dtype = DT_DIR;
    return 1;
}

/** Give a listing the entry `what` with the inode `inode` and the type
 * `type`, its name cut at 31 bytes, and 1; or 0 when `what` is NULL, past
 * the directory's last. */
static int synth_entry(const char *what, uint32_t inode, uint8_t type, char *name, uint64_t *ino,
                       uint8_t *dtype)
{
    if (what == NULL) {
        return 0;
    }
    k_strncpy(name, what, 31);
    name[31] = '\0';
    *ino = inode;
    *dtype = type;
    return 1;
}

/** The `n`th entry, past "." and "..", of row `row` of /dev's directories:
 * its devices present now (`devfs_subdir_dirent`), as `synth_entry` gives
 * one. */
static int synth_dev_dir_dirent(int row, int n, char *name, uint64_t *ino, uint8_t *dtype)
{
    const struct synth_dev_dir *d = synth_dev_dir_at(row);
    const char *dn = NULL;
    uint32_t dino = 0;

    if (!d || !devfs_subdir_dirent(d->name, n, &dn, &dino)) {
        dn = NULL;
    }
    return synth_entry(dn, dino, DT_CHR, name, ino, dtype);
}

/** The `n`th entry of /sys/devices/system/cpu, as `synth_sys_dirent`
 * gives it: a directory for each processor, then the attributes. */
static int synth_sys_cpu_dirent(int n, char *name, uint64_t *ino, uint8_t *dtype)
{
    int key = sysfs_device_key(SYSFS_CPUN, n), dev;

    if (key >= 0) {
        name[0] = 'c';
        name[1] = 'p';
        name[2] = 'u';
        k_itoa(key, name + 3, 28, 10, false);
        *ino = sysfs_ino(SYSFS_CPUN, key, 0);
        *dtype = DT_DIR;
        return 1;
    }
    for (dev = 0; sysfs_device_key(SYSFS_CPUN, dev) >= 0; dev++) {
    }
    n -= dev;
    return synth_entry(sysfs_attr(SYSFS_CPU, n), sysfs_ino(SYSFS_CPU, 0, 1 + n), DT_REG, name, ino,
                       dtype);
}

/**
 * The `n`th entry of a processor's directory `node`, SYSFS_CPUN, of its
 * caches', SYSFS_CPU_CACHE, or of a directory of attribute files, whose
 * key is `key`, as `synth_sys_dirent` gives it: a processor's `topology`
 * and, when it describes its caches, `cache`; a directory `indexM` for each
 * cache; and the attributes `sysfs_attr` lists that are there, of which
 * /sys/class/dmi/id has only the fields the firmware's table holds.
 */
static int synth_sys_attr_dirent(int node, int key, int n, char *name, uint64_t *ino,
                                 uint8_t *dtype)
{
    int i;

    if (node == SYSFS_CPUN) {
        if (n == 1 && !sysfs_device_exists(SYSFS_CPU_CACHE, key)) {
            n = 2;
        }
        return synth_entry(n == 0   ? "topology"
                           : n == 1 ? "cache"
                                    : NULL,
                           sysfs_ino(n == 0 ? SYSFS_CPU_TOPOLOGY : SYSFS_CPU_CACHE, key, 0), DT_DIR,
                           name, ino, dtype);
    }
    if (node == SYSFS_CPU_CACHE) {
        if (n > 15 || !sysfs_device_exists(SYSFS_CPU_CACHE_INDEX, key << 4 | n)) {
            return 0;
        }
        k_strncpy(name, "index", 31);
        k_itoa(n, name + 5, 26, 10, false);
        *ino = sysfs_ino(SYSFS_CPU_CACHE_INDEX, key << 4 | n, 0);
        *dtype = DT_DIR;
        return 1;
    }
    for (i = 0; sysfs_attr(node, i); i++) {
        if (sysfs_attr_present(node, i) && n-- == 0) {
            break;
        }
    }
    return synth_entry(sysfs_attr(node, i), sysfs_ino(node, key, 1 + i), DT_REG, name, ino, dtype);
}

/**
 * The `n`th entry of the radio directory `node` of /sys, whose radio or
 * switch is `key`, as `synth_sys_dirent` gives it: /sys/class/ieee80211 a
 * directory for each radio, a radio's its attributes and `device`, the
 * device its `driver` link and the driver's directory, which is empty,
 * /sys/class/rfkill a directory for each switch, and a switch's its
 * attributes and a `device` link to its radio, or to itself when it has none.
 */
static int synth_sys_radio_dirent(int node, int key, int n, char *name, uint64_t *ino,
                                  uint8_t *dtype)
{
    int i;

    if (node == SYSFS_CLASS_IEEE80211) {
        struct wlan *wlan = net_ns_seen() ? wlan_at((unsigned)n) : 0;
        if (!wlan)
            return 0;
        k_memcpy(name, "phy", 3);
        k_itoa((int)wlan->radio->index, name + 3, 29, 10, 0);
        *ino = sysfs_ino(SYSFS_WLAN_PHY, (int)wlan->radio->index, 0);
        *dtype = DT_DIR;
        return 1;
    }
    if (node == SYSFS_WLAN_PHY) {
        for (i = 0; sysfs_attr(node, i); ++i) {
            if (n == i)
                return synth_entry(sysfs_attr(node, i), sysfs_ino(node, key, i + 1), DT_REG, name,
                                   ino, dtype);
        }
        return synth_entry(n == i ? "device" : NULL, sysfs_ino(SYSFS_WLAN_DEVICE, key, 0), DT_DIR,
                           name, ino, dtype);
    }
    if (node == SYSFS_WLAN_DEVICE) {
        return synth_entry(n == 0   ? "driver"
                           : n == 1 ? sysfs_wlan_driver(key)
                                    : NULL,
                           sysfs_ino(SYSFS_WLAN_DRIVER, key, 0), n ? DT_DIR : DT_LNK, name, ino,
                           dtype);
    }
    if (node == SYSFS_CLASS_RFKILL) {
        int index = rfkill_nth(n);
        if (index < 0)
            return 0;
        k_memcpy(name, "rfkill", 6);
        k_itoa(index, name + 6, 26, 10, 0);
        *ino = sysfs_ino(SYSFS_RFKILL, index, 0);
        *dtype = DT_DIR;
        return 1;
    }
    if (node == SYSFS_RFKILL) {
        if (sysfs_attr(node, n))
            return synth_entry(sysfs_attr(node, n), sysfs_ino(node, key, 1 + n), DT_REG, name, ino,
                               dtype);
        return synth_entry(n == RFKILL_ATTRS ? "device" : NULL,
                           rfkill_phy(key) >= 0 ? sysfs_ino(SYSFS_WLAN_PHY, rfkill_phy(key), 2)
                                                : sysfs_ino(SYSFS_RFKILL, key, 0xFF),
                           DT_LNK, name, ino, dtype);
    }
    return 0;
}

/**
 * The `n`th entry of the /sys directory `node` of the PCI or USB bus, as
 * `synth_sys_dirent` gives it: a device's uevent, and a PCI function's
 * config after it; a link for each device in the bus's `devices`, and a
 * directory for each in the directory the links lead to. 0 past the last
 * and for any other node.
 */
static int synth_sys_bus_dirent(int node, int key, int n, char *name, uint64_t *ino, uint8_t *dtype)
{
    int dev, links, attr;

    if (node == SYSFS_PCI_DEVICE) {
        for (attr = 0; attr < SYSFS_PCI_ATTRS; attr++) {
            if (!sysfs_pci_attr(attr) || !sysfs_pci_attr_present(key, attr)) {
                continue;
            }
            if (n-- == 0) {
                return synth_entry(sysfs_pci_attr(attr), sysfs_ino(node, key, 1 + attr), DT_REG,
                                   name, ino, dtype);
            }
        }
        return synth_entry(n == 0 ? "subsystem" : NULL, sysfs_ino(SYSFS_BUS_PCI, 0, 2), DT_LNK,
                           name, ino, dtype);
    }
    if (node == SYSFS_USB_DEVICE) {
        return synth_entry(n == 0 ? "uevent" : NULL, sysfs_ino(node, key, 1), DT_REG, name, ino,
                           dtype);
    }
    links = node == SYSFS_PCI_LINKS || node == SYSFS_USB_LINKS;
    if (!links && node != SYSFS_PCI_ROOT && node != SYSFS_USB_ROOT) {
        return 0;
    }
    dev = node == SYSFS_PCI_LINKS || node == SYSFS_PCI_ROOT ? SYSFS_PCI_DEVICE : SYSFS_USB_DEVICE;
    key = sysfs_device_key(dev, n);
    if (key < 0) {
        return 0;
    }
    synth_sys_device_name(dev, key, name, 32);
    *ino = sysfs_ino(dev, key, links ? 2 : 0);
    *dtype = links ? DT_LNK : DT_DIR;
    return 1;
}

/**
 * The `n`th entry of the fixed /sys directory `node`, whose device is `key`:
 * its name, inode and type, and 1, or 0 past the last. The fixed
 * directories hold the next ones down, a bus's `devices` a link for each
 * device, the directory the links lead to a directory for each, a device's
 * directory its uevent, a processor's its `topology`, /sys/class/net its
 * interfaces and /sys/class/backlight its backlights, each with its
 * attributes.
 */
static int synth_sys_fixed_dirent(int node, int key, int n, char *name, uint64_t *ino,
                                  uint8_t *dtype)
{
    static const struct {
        int node;
        const char *name;
        int child;
    } fixed[] = {{SYSFS_ROOT, "bus", SYSFS_BUS},
                 {SYSFS_ROOT, "devices", SYSFS_DEVICES},
                 {SYSFS_BUS, "pci", SYSFS_BUS_PCI},
                 {SYSFS_BUS, "usb", SYSFS_BUS_USB},
                 {SYSFS_BUS_PCI, "devices", SYSFS_PCI_LINKS},
                 {SYSFS_BUS_USB, "devices", SYSFS_USB_LINKS},
                 {SYSFS_DEVICES, "pci0000:00", SYSFS_PCI_ROOT},
                 {SYSFS_DEVICES, "usb", SYSFS_USB_ROOT},
                 {SYSFS_DEVICES, "system", SYSFS_SYSTEM},
                 {SYSFS_SYSTEM, "cpu", SYSFS_CPU},
                 {SYSFS_SYSTEM, "memory", SYSFS_MEMORY},
                 {SYSFS_MEMORY, "memory0", SYSFS_MEMORY0},
                 {SYSFS_ROOT, "class", SYSFS_CLASS},
                 {SYSFS_ROOT, "power", SYSFS_POWER},
                 {SYSFS_CLASS, "backlight", SYSFS_CLASS_BACKLIGHT},
                 {SYSFS_CLASS, "net", SYSFS_CLASS_NET},
                 {SYSFS_CLASS, "ieee80211", SYSFS_CLASS_IEEE80211},
                 {SYSFS_CLASS, "rfkill", SYSFS_CLASS_RFKILL},
                 {SYSFS_CLASS, "rtc", SYSFS_CLASS_RTC},
                 {SYSFS_CLASS_RTC, "rtc0", SYSFS_RTC0},
                 {SYSFS_RTC0, "device", SYSFS_RTC_DEVICE},
                 {SYSFS_RTC_DEVICE, "power", SYSFS_RTC_POWER},
                 {SYSFS_ROOT, "block", SYSFS_BLOCK},
                 {SYSFS_ROOT, "dev", SYSFS_DEV},
                 {SYSFS_DEV, "block", SYSFS_DEV_BLOCK},
                 {SYSFS_DEV, "char", SYSFS_DEV_CHAR},
                 {SYSFS_ROOT, "kernel", SYSFS_KERNEL},
                 {SYSFS_KERNEL, "debug", SYSFS_KERNEL_DEBUG},
                 {SYSFS_KERNEL, "mm", SYSFS_KERNEL_MM},
                 {SYSFS_KERNEL_MM, "transparent_hugepage", SYSFS_THP},
                 {SYSFS_CLASS, "drm", SYSFS_CLASS_DRM},
                 {SYSFS_DEVICES, "platform", SYSFS_PLATFORM},
                 {SYSFS_BUS, "platform", SYSFS_BUS_PLATFORM},
                 {SYSFS_BUS_PLATFORM, "devices", SYSFS_PLATFORM_LINKS},
                 {SYSFS_SYSTEM, "node", SYSFS_NODE},
                 {SYSFS_NODE, "node0", SYSFS_NODE0},
                 {SYSFS_SYSTEM, "clocksource", SYSFS_CLOCKSOURCE},
                 {SYSFS_CLOCKSOURCE, "clocksource0", SYSFS_CLOCKSOURCE0},
                 {SYSFS_CLASS, "dmi", SYSFS_CLASS_DMI},
                 {SYSFS_CLASS_DMI, "id", SYSFS_DMI_ID}};
    int i;

    for (i = 0; i < (int)(sizeof(fixed) / sizeof(fixed[0])); i++) {
        if (fixed[i].node == node && n-- == 0) {
            return synth_entry(fixed[i].name, sysfs_ino(fixed[i].child, 0, 0), DT_DIR, name, ino,
                               dtype);
        }
    }
    if (node == SYSFS_CLASS_NET || node == SYSFS_NET_IF || node == SYSFS_NET_STATS) {
        return synth_sys_net_dirent(node, key, n, name, ino, dtype);
    }
    if (SYSFS_IS_RADIO(node)) {
        return synth_sys_radio_dirent(node, key, n, name, ino, dtype);
    }
    if (node == SYSFS_CLASS_BACKLIGHT) {
        return synth_entry(backlight_name(n), sysfs_ino(SYSFS_BACKLIGHT, n, 0), DT_DIR, name, ino,
                           dtype);
    }
    if (node == SYSFS_BLOCK || node == SYSFS_DEV_BLOCK || SYSFS_IS_BLOCK(node)) {
        return synth_sys_block_dirent(node, key, n, name, ino, dtype);
    }
    if (node == SYSFS_CPU) {
        return synth_sys_cpu_dirent(n, name, ino, dtype);
    }
    if (node == SYSFS_CPUN || node == SYSFS_CPU_CACHE || sysfs_attr(node, 0)) {
        return synth_sys_attr_dirent(node, key, n, name, ino, dtype);
    }
    if (node >= SYSFS_REG) {
        return 0;
    }
    return synth_sys_bus_dirent(node, key, n, name, ino, dtype);
}

/**
 * The `n`th entry of the /sys directory `node` of device `key`: first the
 * entries drivers added below it (sysfs.h), in the order of their slots,
 * then the fixed directory's own (`synth_sys_fixed_dirent`); a registered
 * directory has only the first. Its name, inode and type, and 1, or 0 past
 * the last.
 */
static int synth_sys_dirent(int node, int key, int n, char *name, uint64_t *ino, uint8_t *dtype)
{
    int anchor = node == SYSFS_REG ? key : sysfs_anchor_find(node, key);
    int count = anchor > 0 ? sysfs_node_count(anchor) : 0;
    int h, kind;

    if (n < count) {
        h = sysfs_node_nth(anchor, n);
        kind = sysfs_node_kind(h);
        return synth_entry(h > 0 ? sysfs_node_name(h) : NULL, sysfs_ino(SYSFS_REG, h, 0),
                           kind == SYSFS_NODE_ATTR   ? DT_REG
                           : kind == SYSFS_NODE_LINK ? DT_LNK
                                                     : DT_DIR,
                           name, ino, dtype);
    }
    return node == SYSFS_REG ? 0 : synth_sys_fixed_dirent(node, key, n - count, name, ino, dtype);
}

/**
 * Entry `n` of /proc/net, or with `stat` of /proc/net/stat: 1 with its name,
 * inode and type, or 0 past the last. /proc/net has dev, route, arp and tcp,
 * then the files `synth_net_file` gives, then the directory `stat`, which
 * has rt_cache and arp_cache.
 */
static int synth_net_dirent(int stat, int n, char *name, uint64_t *ino, uint8_t *dtype)
{
    static const char *const netfiles[] = {"dev", "route", "arp", "tcp"};
    int count = (int)(sizeof(netfiles) / sizeof(netfiles[0]));
    const char *entry;

    if (stat) {
        if (n > 1) {
            return 0;
        }
        k_strncpy(name, n == 0 ? "rt_cache" : "arp_cache", 31);
        *dtype = DT_REG;
        *ino = procfs_ino_net(n == 0 ? SYNTH_PROC_NETSTAT_RT : SYNTH_PROC_NETSTAT_ARP);
        return 1;
    }
    entry = n < count ? netfiles[n] : synth_net_name(synth_net_file(n - count));
    if (!entry && n > count && synth_net_file(n - count - 1)) {
        k_strncpy(name, "stat", 31);
        *dtype = DT_DIR;
        *ino = procfs_ino_netstatdir();
        return 1;
    }
    if (!entry) {
        return 0;
    }
    k_strncpy(name, entry, 31);
    *dtype = DT_REG;
    if (n == 0) {
        *ino = procfs_ino_netdev();
    } else if (n == 1) {
        *ino = procfs_ino_netroute();
    } else if (n == 2) {
        *ino = procfs_ino_netarp();
    } else if (n == 3) {
        *ino = procfs_ino_nettcp();
    } else {
        *ino = procfs_ino_net(synth_net_file(n - count));
    }
    return 1;
}

/** Entry `n` of /proc/sysvipc: its three files. */
static int synth_sysvipc_dirent(int n, char *name, uint64_t *ino, uint8_t *dtype)
{
    const char *entry = synth_sysvipc_name(SYNTH_PROC_SYSVIPC + n);

    if (!entry) {
        return 0;
    }
    k_strncpy(name, entry, 31);
    *dtype = DT_REG;
    *ino = procfs_ino_sysvipc(SYNTH_PROC_SYSVIPC + n);
    return 1;
}

/** The `n`th entry of /dev past "." and "..", as `synth_dirent_at` lists
 * it: the named mounts, `log`, the descriptors' links, the directories of
 * synthfs's table present now and the devices of devfs. */
static int synth_dev_dirent(int n, char *name, uint64_t *ino, uint8_t *dtype)
{
    const char *dn = NULL;
    uint32_t dino = 0;
    uint32_t dmode = 0;
    uint64_t mino = 0;
    int i, named = 0;

    while (vfs_named_mount(vfs_mount_at(1), named, &dn, &mino)) {
        named++;
    }
    if (n < named && vfs_named_mount(vfs_mount_at(1), n, &dn, &mino)) {
        for (i = 0; i < 30 && dn[i]; i++) {
            name[i] = dn[i];
        }
        name[i] = '\0';
        *ino = mino;
        *dtype = DT_DIR;
        return 1;
    }
    n -= named;
    if (n == 0) {
        k_strncpy(name, "log", 31);
        *ino = DEV_LOG_INO;
        *dtype = DT_LNK;
        return 1;
    }
    n--;
    if (n < 4) {
        static const char *const fdnames[] = {"fd", "stdin", "stdout", "stderr"};
        struct kstat fst;
        struct vpath fvp;

        k_memset(&fvp, 0, sizeof(fvp));
        fvp.pid = SYNTH_PID_SELF;
        fvp.kind = n == 0 ? VNODE_PROC_FD_DIR : VNODE_PROC_FD_LINK;
        fvp.procfd = n - 1;
        for (i = 0; fdnames[n][i]; i++) {
            name[i] = fdnames[n][i];
        }
        name[i] = '\0';
        *ino = synth_stat_node(&fvp, &fst, 0) == 0 ? fst.st_ino : 1u;
        *dtype = n == 0 ? DT_DIR : DT_LNK;
        return 1;
    }
    n -= 4;
    for (i = 0; i < synth_dev_dir_count(); i++) {
        if (devfs_subdir_present(i) && n-- == 0) {
            k_strncpy(name, synth_dev_dir_at(i)->name, 31);
            *ino = synth_dev_dir_at(i)->ino;
            *dtype = DT_DIR;
            return 1;
        }
    }
    if (!devfs_dirent(n, &dn, &dino) || !dn) {
        return 0;
    }
    for (i = 0; i < 30 && dn[i]; i++) {
        name[i] = dn[i];
    }
    name[i] = '\0';
    *ino = dino;
    (void)devfs_stat(dn, &dmode, NULL, NULL);
    *dtype = (uint8_t)VFS_DT_OF(dmode);
    return 1;
}

/* The room for the name of a synthetic directory's entry, the NUL with it. */
#define SYNTH_DIRENT_NAME 256

/**
 * The `n`th entry of /dev past "." and "..", or of the directory of its
 * table in `row` when `row` is not negative, or of the directory made under
 * /dev that `made` is when it is above 0: first the entries
 * `synth_dev_dirent` or `synth_dev_dir_dirent` gives, then those root made
 * there (`devtree_dirent`). 1, or 0 past the last.
 */
static int synth_dev_any_dirent(int row, int made, int n, char *name, uint64_t *ino, uint8_t *dtype)
{
    const char *mn;
    uint32_t mino;
    int table = 0, parent = made > 0 ? made : row >= 0 ? DEVTREE_ROW(row) : DEVTREE_ROOT;

    if (made <= 0) {
        if (row >= 0 ? synth_dev_dir_dirent(row, n, name, ino, dtype)
                     : synth_dev_dirent(n, name, ino, dtype)) {
            return 1;
        }
        while (row >= 0 ? synth_dev_dir_dirent(row, table, name, ino, dtype)
                        : synth_dev_dirent(table, name, ino, dtype)) {
            table++;
        }
    }
    if (!devtree_dirent(parent, n - table, &mn, &mino, dtype)) {
        return 0;
    }
    k_strncpy(name, mn, SYNTH_DIRENT_NAME - 1);
    name[SYNTH_DIRENT_NAME - 1] = '\0';
    *ino = mino;
    return 1;
}

/* The entries of /proc before its processes: name, inode and type. */
static const struct {
    const char *name;
    uint32_t (*inode)(void);
    uint8_t type;
} synth_proc_globals[] = {{"cmdline", procfs_ino_cmdline, DT_REG},
                          {"disk", procfs_ino_disk, DT_REG},
                          {"uptime", procfs_ino_uptime, DT_REG},
                          {"stat", procfs_ino_statsys, DT_REG},
                          {"meminfo", procfs_ino_meminfo, DT_REG},
                          {"cpuinfo", procfs_ino_cpuinfo, DT_REG},
                          {"loadavg", procfs_ino_loadavg, DT_REG},
                          {"diskstats", procfs_ino_diskstats, DT_REG},
                          {"version", procfs_ino_version, DT_REG},
                          {"partitions", procfs_ino_partitions, DT_REG},
                          {"vmstat", procfs_ino_vmstat, DT_REG},
                          {"slabinfo", procfs_ino_slabinfo, DT_REG},
                          {"swaps", procfs_ino_swaps, DT_REG},
                          {"locks", procfs_ino_locks, DT_REG},
#ifdef RACH2_LOCKDEP
                          {"lockdep", procfs_ino_lockdep, DT_REG},
                          {"lockdep_stats", procfs_ino_lockdep_stats, DT_REG},
#endif
                          {"mounts", procfs_ino_mounts, DT_REG},
                          {"net", procfs_ino_netdir, DT_DIR},
                          {"sysvipc", procfs_ino_sysvipcdir, DT_DIR},
                          {"irq", procfs_ino_irqdir, DT_DIR},
                          {"acpi", procfs_ino_acpidir, DT_DIR},
                          {"fs", procfs_ino_fsdir, DT_DIR},
                          {"sys", procfs_ino_sysdir, DT_DIR},
                          {"self", procfs_ino_dir, DT_LNK},
                          {"usb", procfs_ino_usb, DT_REG},
                          {"loop", procfs_ino_loop, DT_REG},
                          {"console", procfs_ino_console, DT_REG},
                          {"audio", procfs_ino_audio, DT_REG},
                          {"interrupts", procfs_ino_interrupts, DT_REG},
                          {"filesystems", procfs_ino_filesystems, DT_REG}};

/** List one message directory, the live message numbers, or the fixed
 * files of /proc/acpi and /proc/fs. */
static int synth_small_proc_dirent(const struct synth_dir *sd, int n, char *name, uint64_t *ino,
                                   uint8_t *dtype)
{
    int v, seen = 0;

    if (sd->kind == VNODE_PROC_ACPI_DIR) {
        return synth_entry(n == 0 ? "call" : NULL, procfs_ino_acpicall(), DT_REG, name, ino, dtype);
    }
    if (sd->kind == VNODE_PROC_FS_DIR) {
        return synth_entry(n == 0 ? "lockwait" : NULL, procfs_ino_lockwait(), DT_REG, name, ino,
                           dtype);
    }
    if (sd->pid >= 0) {
        return synth_entry(n == 0   ? "smp_affinity"
                           : n == 1 ? "effective_affinity"
                                    : NULL,
                           procfs_ino_irq(sd->pid, n + 1), DT_REG, name, ino, dtype);
    }
    for (v = 0; v < 256; v++) {
        if (procfs_irq_exists(v) && seen++ == n) {
            synth_u32_str(name, (uint32_t)v);
            *ino = procfs_ino_irq(v, 0);
            *dtype = DT_DIR;
            return 1;
        }
    }
    return 0;
}

/**
 * The entry at position `*pos` of a synthetic directory, from the directory
 * descriptor's record `sd`, its kind, pid and tid: fills name/ino/dtype and
 * returns 1, or 0 past the end. "." and ".." are at 0 and 1 of every
 * directory. The n-th entry is at position n, except in a process's `fd` and
 * `fdinfo` directories, where a descriptor is at its number plus 2 and the
 * entry is the first at `*pos` or after it (`synth_proc_dirent`); `*pos` is
 * set to the position of the entry given.
 *
 * /dev lists first the mounts that stand in it by name, /dev/shm; then
 * `log`, the link to the syslog daemon's socket; then the caller's
 * descriptors, `fd` and the three standard ones (`synth_classify`), each
 * with the inode lstat reports for it; then the directories of synthfs's
 * table present now (`devfs_subdir_present`); then the devices
 * `devfs_dirent` gives, with the registry's inodes. `devfs_dirent` skips a
 * device that is absent, so a device after an absent one is still listed,
 * and a directory of the table lists its devices present in the same way.
 * Each directory of /dev lists after these the entries root made in it
 * (`synth_dev_any_dirent`), and a directory root made lists those alone.
 * `name` has room for SYNTH_DIRENT_NAME bytes.
 * /dev/pts has an entry for each allocated pseudoterminal, named by its
 * index; /proc/acpi has `call`; a directory
 * of /proc/sys has the entries of the table under it; and /proc has its
 * files, then a directory for each process. The other directories have
 * functions of their own.
 */
static int synth_dirent_at(const struct synth_dir *sd, int *pos, char *name, uint64_t *ino,
                           uint8_t *dtype)
{
    int kind = sd->kind;
    int n = *pos;

    if (n == 0) {
        name[0] = '.';
        name[1] = '\0';
        *ino = 1;
        *dtype = DT_DIR;
        return 1;
    }
    if (n == 1) {
        name[0] = '.';
        name[1] = '.';
        name[2] = '\0';
        *ino = 1;
        *dtype = DT_DIR;
        return 1;
    }
    n -= 2;

    if (kind == VNODE_DEV_DIR) {
        return synth_dev_any_dirent(-1, 0, n, name, ino, dtype);
    }
    if (kind == VNODE_DEV_SUBDIR) {
        return synth_dev_any_dirent(sd->pid, 0, n, name, ino, dtype);
    }
    if (kind == VNODE_DEV_MADE) {
        return synth_dev_any_dirent(-1, sd->pid, n, name, ino, dtype);
    }
    switch (kind) {
    case VNODE_PROC_IRQ_DIR:
    case VNODE_PROC_ACPI_DIR:
    case VNODE_PROC_FS_DIR:
        return synth_small_proc_dirent(sd, n, name, ino, dtype);
    default:
        break;
    }
    if (kind == VNODE_PTS_DIR) {
        int i, c = 0;
        for (i = 0; i < pty_index_limit(); i++) {
            if (pty_is_allocated(i)) {
                if (c == n) {
                    synth_u32_str(name, (uint32_t)i);
                    *ino = PTS_INO_BASE + (uint32_t)i;
                    *dtype = DT_CHR;
                    return 1;
                }
                c++;
            }
        }
        return 0;
    }
    if (kind == VNODE_SYS_DIR) {
        return synth_sys_dirent(sd->pid, sd->tid, n, name, ino, dtype);
    }
    if (kind == VNODE_PROC_SYS_DIR) {
        int i, c = 0;

        for (i = 1; i < synth_sys_count(); i++) {
            if (synth_sys_parent(i) == sd->pid && c++ == n) {
                const char *entry = synth_sys_name(i);
                int k;

                for (k = 0; entry[k] != '\0'; k++) {
                    name[k] = entry[k];
                }
                name[k] = '\0';
                *ino = procfs_ino_sys(i);
                *dtype = synth_sys_text(i) ? DT_REG : DT_DIR;
                return 1;
            }
        }
        return 0;
    }
    if (kind == VNODE_PROC_SYSVIPC_DIR) {
        return synth_sysvipc_dirent(n, name, ino, dtype);
    }
    if (kind == VNODE_PROC_NET_DIR || kind == VNODE_PROC_NET_STAT_DIR) {
        return synth_net_dirent(kind == VNODE_PROC_NET_STAT_DIR, n, name, ino, dtype);
    }
    if (kind == VNODE_PROC_DIR) {
        int count = (int)(sizeof(synth_proc_globals) / sizeof(synth_proc_globals[0]));

        if (n < count) {
            int i;
            for (i = 0; synth_proc_globals[n].name[i] != '\0'; i++)
                name[i] = synth_proc_globals[n].name[i];
            name[i] = '\0';
            *dtype = synth_proc_globals[n].type;
            *ino = synth_proc_globals[n].inode();
            return 1;
        }
        {
            int p = procfs_nth_pid(n - count);
            if (p < 0) {
                return 0;
            }
            synth_u32_str(name, (uint32_t)p);
            *ino = procfs_ino_pid(procfs_resolve_pid(p));
            *dtype = DT_DIR;
            return 1;
        }
    }
    if (kind == VNODE_PROC_PID || kind == VNODE_PROC_TASK_DIR || kind == VNODE_PROC_FD_DIR ||
        kind == VNODE_PROC_NS_DIR || kind == VNODE_PROC_FDINFO_DIR) {
        int found = synth_proc_dirent(sd, &n, name, ino, dtype);

        *pos = n + 2;
        return found;
    }
    return 0;
}

/* The directory record layouts: native 64-bit, i386, and old readdir. */
#define DIRENT_NATIVE 0
#define DIRENT_COMPAT 1
#define DIRENT_OLD    2

/**
 * Copy the record of the directory entry `de` in `format` to `buf` of
 * `count` bytes: a native `linux_dirent64`, an i386 `linux_dirent` with
 * the type in its last byte, or an old readdir `old_linux_dirent`, whose
 * length field holds the name's length. `here` is the description's offset
 * at the entry and `next` the one past it; old readdir's offset field takes
 * the former and getdents' d_off the latter. Returns the record's size, or
 * -EINVAL when it does not fit, -EOVERFLOW for an inode number an i386
 * record cannot hold, or -EFAULT, with nothing written. The caller holds
 * the directory's description lock and moves its offset only after this
 * copy succeeds, so a record not copied is read again by the next call.
 */
static long dirent_put(char *buf, uint32_t count, const struct vfs_dirent *de, uint64_t here,
                       uint64_t next, int format)
{
    union {
        struct linux_dirent64 native;
        uint8_t bytes[VFS_NAME_MAX + SYNTH_DIRENT_NAME + 32];
    } rec;
    uint32_t name_len = (uint32_t)k_strlen(de->name);
    uint32_t len, word;
    uint16_t half;

    if (format == DIRENT_NATIVE) {
        len = (19u + name_len + 1u + 7u) & ~7u;
    } else {
        if (de->ino > 0xFFFFFFFFu) {
            return -EOVERFLOW;
        }
        len = format == DIRENT_OLD ? 10u + name_len + 1u : (10u + name_len + 2u + 3u) & ~3u;
    }
    if (len > count) {
        return -EINVAL;
    }
    k_memset(rec.bytes, 0, len);
    if (format == DIRENT_NATIVE) {
        rec.native.d_ino = de->ino;
        rec.native.d_off = (int64_t)next;
        rec.native.d_reclen = (uint16_t)len;
        rec.native.d_type = (uint8_t)de->type;
        k_memcpy(rec.native.d_name, de->name, name_len + 1u);
    } else {
        word = (uint32_t)de->ino;
        k_memcpy(rec.bytes, &word, sizeof(word));
        word = (uint32_t)(format == DIRENT_OLD ? here : next);
        k_memcpy(rec.bytes + 4, &word, sizeof(word));
        half = (uint16_t)(format == DIRENT_OLD ? name_len : len);
        k_memcpy(rec.bytes + 8, &half, sizeof(half));
        k_memcpy(rec.bytes + 10, de->name, name_len + 1u);
        if (format == DIRENT_COMPAT) {
            rec.bytes[len - 1u] = (uint8_t)de->type;
        }
    }
    return copy_to_user((void __user *)buf, rec.bytes, len) == 0 ? (long)len : -EFAULT;
}

/**
 * getdents64, getdents or old readdir, by `format`, on the synthetic
 * directory descriptor `e`: the records of the entries `synth_dirent_at`
 * gives from the description's offset on, packed into the user buffer
 * `buf` of `count` bytes as `getdents` packs a mounted directory's, each
 * record's offset the position after its entry and the description's
 * offset moved past each record copied (`dirent_put`). Old readdir copies
 * one record and returns 1. Returns the bytes written, 0 at the end, or,
 * when the first record is not copied, `dirent_put`'s -EINVAL, -EOVERFLOW
 * or -EFAULT. The caller holds the description's lock.
 */
static int32_t synth_getdents(fd_entry_t *e, char *buf, uint32_t count, int format)
{
    static const struct synth_dir none = {VNODE_ENOENT, 0, 0, 0, {0, {0, 0, 0, 0}, 0, 0, NULL}};
    const struct synth_dir *sd = e->priv ? (const struct synth_dir *)e->priv : &none;
    uint32_t written = 0;
    int idx = (int)e->ofd->offset;
    struct vfs_dirent de;
    uint8_t dtype;

    while (synth_dirent_at(sd, &idx, de.name, &de.ino, &dtype)) {
        long n;

        de.type = dtype;
        n = dirent_put(buf + written, count - written, &de, e->ofd->offset, (uint64_t)(idx + 1),
                       format);

        if (n < 0) {
            return written > 0 ? (int32_t)written : (int32_t)n;
        }
        written += (uint32_t)n;
        idx++;
        e->ofd->offset = idx;
        if (format == DIRENT_OLD) {
            return 1;
        }
    }
    return (int32_t)written;
}

/**
 * getdents64's answer for the directory `ofd` is open on at an offset past
 * 0xFFFFFFFF, which only a type whose cursor is a byte (`dir_seeks_bytes`)
 * takes and which lies past any directory a 32-bit cursor reaches: the
 * type's `readdir` is asked at 0xFFFFFFFF all the same, so that a failed
 * mount answers -EIO and a removed directory -ENOENT, as Linux's
 * `iterate_dir` checks both before the listing's end (fs/readdir.c:105),
 * and anything else is 0, the end, with the offset kept. The caller holds
 * the description's lock.
 */
static int dir_past_end(struct open_file *ofd)
{
    struct vfs_dirent de;
    uint32_t cursor = 0xFFFFFFFFu;
    int got = vfs_readdir(ofd->vmnt, ofd->fhandle, &cursor, &de);

    return got < 0 ? got : 0;
}

/**
 * getdents64(2): the descriptor, the buffer and its size. Packs a struct
 * linux_dirent64 for each entry into the buffer from the description's
 * offset on, and moves the offset past each. A record's `d_type` is the type
 * the file system gives the entry, DT_UNKNOWN where it gives none, and never
 * DT_REG for anything but a regular file. Returns the bytes written, 0 only
 * at the end of the directory, or a negative errno: -EFAULT for a NULL or
 * bad buffer of a nonzero size; -EBADF for a descriptor that is not open,
 * is an O_PATH one, or is on a KRFFS root directory removed since
 * (`fd_node_check`); -ENOTDIR for one that is no directory; -EINVAL when the
 * first record does not fit, as for a size of 0 before the end; -EIO before
 * the root file system is mounted; or, with nothing written, the file
 * system's error. A size of 0 checks no buffer, as on Linux.
 *
 * Each record is built in kernel memory and copied out, and the whole buffer
 * is checked and faulted in first, so a copy part way through the batch does
 * not meet an unmapped page. A directory of /dev, /proc or /sys lists from
 * the process table and the device registry (`synth_getdents`).
 *
 * Any other directory is read under the description's lock through its
 * type's `readdir`, whose cursor is the type's own: on the KRFFS root the
 * byte offset of the last node returned plus one, and on a mounted type
 * whatever its `readdir` keeps. An offset past 0xFFFFFFFF lies past the
 * directory's end (`dir_past_end`).
 *
 * The offset takes the cursor only with the record copied, so an entry that
 * does not fit is read again by the next call. A cursor is not a count of
 * entries: `rm -rf` reads a batch of names, unlinks them and reads again,
 * and a cursor that counted entries would skip as many survivors as the
 * first batch removed, and leave a directory rm reports as not empty
 * (tests/unlinkdirprobe.c).
 *
 * On the KRFFS root a removal moves no other node:
 * `krffs_remove_reserved_node` only widens free extents, and the allocator
 * splits a free node and does not shift a reserved one, so a listing
 * interleaved with removals sees every surviving entry once. Growing a file
 * can move its node, when the extent after it is not free and large enough
 * (`krffs_resize_reserved_node`), and a file made during a listing may take
 * the place a removed one freed; a listing in progress may miss such an
 * entry or see it twice, a concurrent change of the directory that POSIX
 * leaves unspecified.
 */
static long getdents(int fd_num, char *buf, uint32_t count, int format)
{
    fd_entry_t *entry;
    uint32_t bytes_written = 0;

    if (!current_proc) {
        return -EFAULT;
    }
    if (format == DIRENT_NATIVE && count > 0 &&
        (!buf || !user_buffer_ok((uintptr_t)buf, count, 1))) {
        return -EFAULT;
    }

    entry = fd_get(current_proc->fds, fd_num);
    if (!entry || (entry->flags & FD_FLAG_PATH)) {
        return -EBADF;
    }

    if (entry->type == FD_TYPE_SYNTH_DIR) {
        long n;

        if (!entry->ofd) {
            return -EBADF;
        }

        fd_ofd_lock(entry->ofd);
        n = synth_getdents(entry, buf, count, format);
        fd_ofd_unlock(entry->ofd);
        return n;
    }

    if (entry->type != FD_TYPE_DIR) {
        return -ENOTDIR;
    }

    if (!vfs_root_mounted()) {
        return -EIO;
    }
    if (fd_node_check(entry) != 0) {
        return -EBADF;
    }

    {
        struct open_file *ofd = entry->ofd;
        uint32_t cursor;

        if (!ofd || !ofd->vmnt) {
            return -EBADF;
        }
        fd_ofd_lock(ofd);
        if (ofd->offset > 0xFFFFFFFFull) {
            int rc = dir_past_end(ofd);

            fd_ofd_unlock(ofd);
            return rc;
        }
        cursor = (uint32_t)ofd->offset;
        for (;;) {
            struct vfs_dirent de;
            long n;
            int got;

            de.type = VFS_DT_UNKNOWN;
            got = vfs_readdir(ofd->vmnt, ofd->fhandle, &cursor, &de);
            if (got == 0) {
                break;
            }
            if (got < 0) {
                fd_ofd_unlock(ofd);
                return (bytes_written > 0) ? (int32_t)bytes_written : got;
            }
            n = dirent_put(buf + bytes_written, count - bytes_written, &de, ofd->offset, cursor,
                           format);
            if (n < 0) {
                fd_ofd_unlock(ofd);
                return bytes_written > 0 ? (long)bytes_written : n;
            }
            bytes_written += (uint32_t)n;
            ofd->offset = cursor;
            if (format == DIRENT_OLD) {
                bytes_written = 1;
                break;
            }
        }
        fd_ofd_unlock(ofd);
    }

    return (int32_t)bytes_written;
}

/** Read Linux's 64-bit directory records under the description lock: without
 * the big lock for a directory `file_vfs_unlocked` admits, whose `readdir`
 * fills a kernel record, as Linux's `iterate_dir` takes the directory's
 * shared lock alone (fs/readdir.c:101), and under it for any other. */
long sys_getdents64(isr_params_t *params)
{
    int fd = (int)SC_ARG(params, 0);
    int big = file_read_lock(current_proc ? fd_get(current_proc->fds, fd) : NULL);
    long rc = getdents(fd, (char *)SC_ARG(params, 1), USER_LEN32(SC_ARG(params, 2)), DIRENT_NATIVE);

    file_ops_unlock(big);
    return rc;
}

/** Read i386 directory records, one old record when `old` is nonzero. */
long ksys_getdents32(int fd, uintptr_t buf, uint32_t count, int old)
{
    return getdents(fd, (char *)buf, old ? 0xFFFFFFFFu : count, old ? DIRENT_OLD : DIRENT_COMPAT);
}

/**
 * dup3(2): the old descriptor, the new one and the flags. `sys_dup2`'s
 * answer, except that `oldfd` equal to `newfd` is -EINVAL, and O_CLOEXEC,
 * the one flag taken, makes the new descriptor close-on-exec; another flag
 * is -EINVAL.
 */
long sys_dup3(isr_params_t *params)
{
    int oldfd = (int)SC_ARG(params, 0);
    int newfd = (int)SC_ARG(params, 1);
    uint32_t flags = (uint32_t)SC_ARG(params, 2);
    int32_t r;

    if (oldfd == newfd) {
        return -EINVAL;
    }
    if (flags & ~(uint32_t)O_CLOEXEC) {
        return -EINVAL;
    }
    r = (int32_t)dup_to(oldfd, newfd, (flags & O_CLOEXEC) != 0);
    return r;
}

/**
 * The kernel path `path`, relative to the working directory unless it is
 * absolute, as a canonical absolute path in `buf` of VFS_PATH_MAX bytes, with
 * `*end` how it named a directory by its end and a last "." put back, as
 * `copy_path_at` does for a user path. Returns its length, or -ENOENT,
 * -ENAMETOOLONG.
 */
static int kernel_path_cwd(char *buf, const char *path, int *end)
{
    uint32_t len = (uint32_t)k_strlen(path);
    int r;

    if (len == 0) {
        return -ENOENT;
    }
    if (len >= VFS_PATH_MAX) {
        return -ENAMETOOLONG;
    }
    *end = path_names_dir(path, (int)len);
    k_memcpy(buf, path, len + 1u);
    if (buf[0] != '/') {
        r = path_prepend(buf, VFS_PATH_MAX, len, proc_cwd(current_proc));
        if (r < 0) {
            return r;
        }
    }
    r = canonicalize_abs(buf);
    return *end == PATH_END_DOT ? path_put_dot(buf, r) : r;
}

/**
 * The answers for the name of the socket file a bind makes, `path`, an
 * absolute canonical path that named a directory by its end as `end` says,
 * once the walk to its parent is made and before the file is: `make_dir_end`'s
 * for such an end, -EEXIST for the root (`new_name_parent`), and -EEXIST for
 * a name that exists (`new_name_taken`), before the -EROFS of a read-only
 * mount and of /dev, /proc and /sys that `open_create` would give, as on
 * Linux. 0 when the name may be made.
 */
static int32_t bind_name_check(const char *path, int end)
{
    struct vfs_ref parent;
    char leaf[VFS_NAME_MAX + 1];
    int32_t rc = new_name_parent(path, &parent, leaf, sizeof(leaf));

    if (rc == 0) {
        rc = make_dir_end(&parent, leaf, end, path);
        if (rc == 0) {
            rc = new_name_taken(&parent, leaf, path);
        }
        vfs_ref_release(&parent);
    }
    return rc;
}

/**
 * Make the socket file an AF_UNIX bind names, from the working directory
 * unless the path is absolute (`kernel_path_cwd`). The file is made as
 * O_CREAT with O_EXCL makes one, with the socket type bits; the hold its
 * creation gave is kept for the socket's life, as a working directory keeps
 * its own, and counted on the mount, so an unmount of it is -EBUSY while the
 * socket is bound. A name that exists, or one that names a directory by its
 * end, makes nothing (`bind_name_check`), as on Linux: -ENOENT after a slash
 * for a name nothing has, and -EADDRINUSE, which a name that exists answers,
 * for one that does, for a last "." or "..", and for the root, before a
 * read-only mount's -EROFS.
 */
int32_t fs_socket_bind(const char *path, uint32_t mode, struct vfs_ref *ref, uint32_t *ino)
{
    char *buf = path_buf_get();
    struct vfs_stat st;
    int end;
    int rc;

    if (!buf) {
        return -ENOMEM;
    }
    rc = kernel_path_cwd(buf, path, &end);
    if (rc >= 0) {
        rc = bind_name_check(buf, end);
    }
    if (rc >= 0) {
        rc = open_create(buf, O_EXCL, S_IFSOCK | (mode & 07777u), ref, NULL);
        if (rc == 0) {
            vfs_walk_unlock(ref->mnt);
            rc = vfs_getattr(ref->mnt, ref->node, &st);
            if (rc == 0) {
                *ino = (uint32_t)st.ino;
                vfs_mount_get(ref->mnt);
                vfs_leave(ref->mnt);
            } else {
                vfs_ref_release(ref);
            }
        }
    }
    path_buf_put(buf);
    return (rc == 1 || rc == -EEXIST) ? -EADDRINUSE : rc;
}

/** Release the node's hold and the mount's count that `fs_socket_bind`
 * kept, and with the last count unmount the mount if no live namespace sees
 * it, as the close of its last description does (`vfs_reap_detached`). */
void fs_socket_unbind(struct vfs_ref *ref)
{
    struct vfs_mount *mnt = ref->mnt;

    vfs_ref_put(ref);
    if (vfs_mount_put(mnt) == 0) {
        vfs_reap_detached();
    }
}

/**
 * Find the socket file a connect or a send names, from the working
 * directory unless the path is absolute. A name that names a directory by
 * its end must reach one (`resolve_named`), so a socket file so named is
 * -ENOTDIR, as on Linux. The type is checked before the write permission:
 * a file that is not a socket is -ECONNREFUSED whatever its mode.
 */
int32_t fs_socket_lookup(const char *path, struct vfs_mount **mnt, uint32_t *ino)
{
    char *buf = path_buf_get();
    struct vfs_ref ref;
    struct vfs_stat st;
    int end;
    int rc;

    if (!buf) {
        return -ENOMEM;
    }
    rc = kernel_path_cwd(buf, path, &end);
    if (rc >= 0) {
        rc = resolve_named(buf, 0, end, &ref);
    }
    path_buf_put(buf);
    if (rc != 0) {
        return rc;
    }
    rc = vfs_getattr(ref.mnt, ref.node, &st);
    if (rc == 0 && (st.mode & S_IFMT) != S_IFSOCK) {
        rc = -ECONNREFUSED;
    }
    if (rc == 0) {
        rc = mnt_is_root(ref.mnt) ? (int)cred_access_node((krffs_node_t *)ref.node, CRED_W, 0)
                                  : (int)cred_access_ref(&ref, CRED_W, 0);
    }
    if (rc == 0) {
        *mnt = ref.mnt;
        *ino = (uint32_t)st.ino;
    }
    vfs_ref_release(&ref);
    return rc;
}

/**
 * Write a shared file mapping's bytes back: -ENODEV for a descriptor that
 * is not open on a file. It takes the mount's namespace lock, which a type
 * that moves a file's bytes needs, or the file's own on a type that sets
 * `file_locked`, and not the description's lock, as `fs_read_at` does.
 */
int32_t fs_write_at(fd_entry_t *e, const void *buf, uint32_t count, uint64_t off)
{
    struct open_file *ofd = e->ofd;
    struct vfs_ref file;
    int32_t n;
    int locked;

    if (e->type != FD_TYPE_FILE || !ofd || !ofd->vmnt) {
        return -ENODEV;
    }
    file.mnt = ofd->vmnt;
    file.node = ofd->vnode;
    locked = ofd->vmnt->type->ns_serialised || vfs_file_locked(ofd->vmnt);
    if (locked) {
        vfs_node_lock(&file);
    }
    n = vfs_write(ofd->vmnt, ofd->fhandle, buf, count, off);
    if (locked) {
        vfs_node_unlock(&file);
    }
    return n;
}

/**
 * Read a file mapping's bytes: -ENODEV for a descriptor that is not open on
 * a file. It takes the mount's namespace lock, which a type that moves a
 * file's bytes needs, and not the description's lock: a read(2) into a
 * mapping of the file being read faults here while that lock is held.
 */
int32_t fs_read_at(fd_entry_t *e, void *buf, uint32_t count, uint64_t off)
{
    struct open_file *ofd = e->ofd;
    int32_t n;

    if (e->type != FD_TYPE_FILE || !ofd || !ofd->vmnt) {
        return -ENODEV;
    }
    if (ofd->vmnt->type->ns_serialised) {
        vfs_ns_lock(ofd->vmnt);
    }
    n = vfs_read(ofd->vmnt, ofd->fhandle, buf, count, off);
    if (ofd->vmnt->type->ns_serialised) {
        vfs_ns_unlock(ofd->vmnt);
    }
    return n;
}
