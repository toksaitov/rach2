/*
 * The file system arm of the system call layer: descriptors, paths and the
 * credential model, open, read and write, the stat family, links, mounts
 * and directories. Registered by syscall.c; what it shares with the other
 * arms is in syscall_internal.h.
 */
#include "../kernel.h"
#include "mm/kheap.h"
#include "klog.h"
#include "syscall.h"
#include "power.h"
#include "serial.h"
#include "panic.h"
#include "kio.h"
#include "kbd.h"
#include "input.h"
#include "mouse.h"
#include "proc/process.h"
#include "proc/sched.h"
#include "proc/tgroup.h"
#include "proc/elf.h"
#include "proc/uimage.h"
#include "proc/tls.h"
#include "tables/gdt.h"
#include "lib/krandom.h"
#include "lib/kerrno.h"
#include "lib/ksignal.h"
#include "fs/fd.h"
#include "fs/file.h"
#include "fs/lock.h"
#include "fs/pathbuf.h"
#include "ipc/pipe.h"
#include "io/poll.h"
#include "tty/tty.h"
#include "tty/pty.h"
#include "rtc.h"
#include "block/loop.h"
#include "fs/krffs.h"
#include "fs/krffsmount.h"
#include "fs/fat32.h"
#include "fs/exfat.h"
#include "fs/devfs.h"
#include "fs/synthfs.h"
#include "fs/vfs.h"
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
#include "display/vga_gfx.h"
#include "speaker.h"
#include "audio/audio.h"
#include "network/socket/socket.h"
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
#include "mm/filemap.h"
#include "mm/mm.h"
#include "proc/coredump.h"
#include "syscall_internal.h"

/* The pool backing FD_TYPE_SYNTH_DIR descriptors. A 32-bit pid does not fit
 * packed in the fd's priv pointer alongside the kind without truncation, so the
 * (kind, pid, tid) live here (struct synth_dir is in synthfs.h); priv points at
 * a record from the heap, freed when the last descriptor referencing it closes
 * (see fd_release_backing). */
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

/** The classified path of the synthetic directory a descriptor's record `sd`
 * is of, or of no entry for a descriptor with no record. */
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
 * node as a descriptor does, and count as one holder more. The ends of a FIFO
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
        for (i = 0; i < fd_table_cap(pr->fds); i++) {
            fd_entry_t *e = &pr->fds->entries[i];

            if (e != except && e->type == FD_TYPE_FILE && e->node == node) {
                held++;
            }
        }
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
 * Free `node`, an orphan of the KRFFS root whose last holder is `e`, once the
 * locks the description of `e` owns are gone. A lock of the root is keyed by
 * its node's place (`fd_lock_key`), and a node the allocator makes there
 * later must meet none of them; the description's own end drops them only
 * after this, when `fd_free` lets its last reference go.
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
 * frees its orphan, and a FIFO's ends are its descriptors.
 */
static void fd_fifo_orphan(const fd_entry_t *e)
{
    krffs_node_t *fifo;

    if (!e->ofd || !e->ofd->vmnt || !krffs_fs || !mnt_is_root(e->ofd->vmnt)) {
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
 * Take the lock the release of `e`, a descriptor or a copy of one, needs when
 * the release may free a node of the KRFFS root that lost its name: the
 * root's namespace lock, for a file of the root, or for an end of a FIFO of
 * the root or the reservation of an open of one. The mount locked, or NULL,
 * with nothing taken, for any other descriptor.
 *
 * The unlink and the rename of a file hold that lock while they count its
 * holders and take its name, and every allocation on the root holds it, so
 * under it the release's count of the holders and its reclaim of the node,
 * which marks the node's extent free and may sleep reading a neighbouring
 * header, come with no count and no allocation between them. A holder that
 * the count finds in a list of its own, a file mapping or a descriptor in
 * flight, takes the lock before it leaves that list, as the address space
 * that runs the file does (`fs_exe_lock`): until then a release of the
 * file's other holders counts it. The release takes the lock again, which
 * its holder may do. May sleep; no caller holds the namespace lock of
 * another mount or the lock of a description on a mount.
 */
struct vfs_mount *fd_release_lock(const fd_entry_t *e)
{
    struct vfs_mount *root = vfs_root_mount();
    int orphanable;

    if (!krffs_fs) {
        return NULL;
    }
    if (e->type == FD_TYPE_FILE) {
        orphanable = e->node && e->node != krffs_fs->node;
    } else {
        orphanable = (e->type == FD_TYPE_PIPE || e->type == FD_TYPE_NONE) && e->ofd &&
                     e->ofd->vmnt == root && mnt_is_root(root);
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

    if (!locked) {
        return;
    }
    if (e->type == FD_TYPE_FILE) {
        if (e->node->name[0] == '\0' && fd_node_holders(e->node, e) == 0) {
            fd_free_orphan(e, e->node);
            e->node = NULL;
        }
    } else {
        fd_fifo_orphan(e);
    }
    vfs_ns_unlock(locked);
}

/**
 * Whether any descriptor anywhere has `node`. For callers outside this file.
 *
 * The debug shell removes and renames through the file system directly, below
 * the syscall layer that owns the holder rule, so it asks here rather than
 * carrying its own copy of the answer.
 */
int fs_node_is_held(const krffs_node_t *node)
{
    return fd_node_holders(node, NULL) > 0;
}

/**
 * Repoint every holder from one node to another, for callers outside this
 * file. The debug shell writes through the file system directly, and a write
 * that grows a file past its extent relocates it; the sweep over
 * descriptors, descriptions, working directories and mounts belongs to the
 * VFS, which is the one place that knows every holder kind.
 */
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
    if (e->ofd && e->ofd->vmnt) {
        key->mnt = e->ofd->vmnt;
        key->ino =
            mnt_locks_by_node(e->ofd->vmnt) ? (uint64_t)(uintptr_t)e->ofd->vnode : e->ofd->ino;
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

/**
 * Drop the record locks of the calling thread's descriptor table on what `e`
 * is open on, when `e` is one of the table's descriptors, as POSIX has every
 * close do; the table owns them for every thread and process that shares it
 * (`lock.h`), so a close by any of them drops them all. The copy of an entry
 * a file mapping or a message in flight keeps is not a descriptor of the
 * table (`fd_in_table`), and its release drops none; nor does the close of
 * an O_PATH descriptor, which Linux does not count as a close of the file.
 */
static void fd_close_locks(const fd_entry_t *e)
{
    struct lock_key key;

    if (!lock_any() || !e->ofd || (e->flags & FD_FLAG_PATH) || !current_proc ||
        !fd_in_table(current_proc->fds, e)) {
        return;
    }
    fd_lock_key(e, &key);
    lock_close(&key);
}

/**
 * Release every backing reference a descriptor holds before its slot is
 * freed, the table's record locks apart.
 *
 * Drops framebuffer ownership, the per-fd socket reference, and an ops-backed
 * object (pipe/tty/pty). Every fd-teardown site routes through this, most of
 * them by `fd_release_backing`, so none can forget a backing kind as new ones
 * are added. It does NOT free the slot; the caller does that with fd_free.
 *
 * An ops-backed object rides the shared open-file description: dup/dup2/fork
 * copy the ops/priv pointers and bump the OFD refcount, so the object must
 * be released exactly once, when its LAST descriptor closes (the final OFD
 * reference, refcount about to reach 0). The socket reference, by contrast,
 * is counted per descriptor (socket_ref on dup/fork), so it is always
 * dropped here. No descriptor is both an ops object and a socket.
 *
 * A KRFFS node is a backing kind like any other here. An orphan, a reserved
 * node no path names, is freed by its last holder, under the root's
 * namespace lock (`fd_release_orphan`), and this runs while the closing
 * slot is still populated, which is what the count's `except` is for: the
 * closing descriptor is skipped, not cleared first, so it is counted by
 * every other release until its own has the lock.
 */
static void fd_release_objects(fd_entry_t *e)
{
    fd_release_orphan(e);
    /* A synthetic directory fd's pooled cookie, or an O_PATH leaf's node, is
     * freed when its last descriptor closes (dup/fork share it via the OFD
     * refcount, like an ops backing). */
    if ((e->type == FD_TYPE_SYNTH_DIR || e->type == FD_TYPE_SYNTH_PATH) && e->priv && e->ofd &&
        e->ofd->refcount == 1) {
        kfree(e->priv);
        e->priv = NULL;
    }
    /* Restore text mode only if THIS process actually owns the framebuffer, so
     * a fork child that merely inherited the fb descriptor cannot clear the
     * parent's graphics mode when the child closes the fd or exits. */
    if (e->type == FD_TYPE_FRAMEBUFFER && current_proc && fb_owner_pid == current_proc->tgid &&
        e->ofd && e->ofd->refcount == 1) {
        vga_mode13h_exit();
        fb_owner_pid = 0;
    }
    if (e->type == FD_TYPE_SOCKET && e->sock) {
        socket_unref(e->sock);
    }
    if (e->ops && e->ops->close && e->ofd && e->ofd->refcount == 1) {
        e->ops->close(e);
    }
}

/**
 * Release every backing reference a descriptor holds before its slot is
 * freed (`fd_release_objects`), and first, for a close of a descriptor of
 * the calling thread's table, the table's record locks on the file
 * (`fd_close_locks`): the close, the dup2 replacement of an open target, the
 * execve CLOEXEC sweep, and the release of the table by its last sharer.
 */
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
 * and a relocation moves the description's node (`fd_ofd_repoint`). NULL
 * when the heap has no room or the pin fails.
 */
struct open_file *fs_exe_open(const struct vfs_ref *ref)
{
    struct open_file *ofd = fd_ofd_new();

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
    if (!mnt_is_root(ofd->vmnt)) {
        return NULL;
    }
    vfs_ns_lock(ofd->vmnt);
    return ofd->vmnt;
}

/**
 * Let go of `ofd`, the description an address space kept its program's file
 * by, once that address space is off the list `mm_exe_holds` reads, with
 * `locked` held as `fs_exe_lock` returned it: a file of the KRFFS root that
 * lost its name is freed when nothing else holds it, as the release of a
 * file's last descriptor frees one (`fd_release_objects`, which takes the
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
        fd_release_objects(&e);
        vfs_ns_unlock(locked);
    }
    fd_ofd_unref(ofd);
}

/**
 * The description an ops-backed read or write of `entry` holds for its
 * length, as lseek holds it to move the offset, or NULL. A raw disk window's
 * transfer uses `raw_offset`; a /proc file's the offset, a byte of its text
 * or an address of /proc/PID/mem; and a log device's the offset, a byte of
 * the kernel log's ring or of the screen log's text
 * (`devfs_fd_moves_offset`). Each may sleep before it sets the offset past
 * what it moved, in a disk request, or in a fault on the caller's buffer or
 * in bringing in a page of a process. A seek through a descriptor that
 * shares the description must wait for it or be lost, and a second transfer
 * must wait to start from where the first leaves the offset. Linux holds
 * such a file's position lock across read, write and lseek alike. pread64
 * and pwrite64 hold nothing, since they work on a copy (`pio_view`).
 */
static struct open_file *ops_offset_held(fd_entry_t *entry)
{
    return devfs_fd_moves_offset(entry) || entry->type == FD_TYPE_PROC ? entry->ofd : NULL;
}

/** Dispatch an ops-backed write, holding the description of one whose write
 * uses its offset (`ops_offset_held`). */
static int32_t write_ops(fd_entry_t *entry, const char __user *buf, uint32_t count)
{
    struct open_file *held = ops_offset_held(entry);
    int32_t rc;

    if (!(entry->flags & FD_FLAG_WRITE)) {
        return -EBADF;
    }
    if (held) {
        fd_ofd_lock(held);
    }
    rc = entry->ops->write ? entry->ops->write(entry, buf, count) : -EBADF;
    if (held) {
        fd_ofd_unlock(held);
    }
    return rc;
}

/**
 * Write `count` bytes of user buffer `buf` to the file of a mounted type or
 * of the KRFFS root that `entry` is open on: at the description's offset,
 * which moves past what was written, or at `*at` for pwrite64, which leaves
 * it where it was.
 *
 * With O_APPEND every write lands at the current end of the file, whatever
 * the offset says and whatever `at` asks for, as Linux's pwrite does. The
 * file system reads the user buffer directly, so its pages are range-checked
 * and faulted in first. A file that backs a loop device is reached through
 * that device and nowhere else, since the file system mounted on it may not
 * have its image change underneath: that is asked before the description is
 * locked, since a return from under the lock leaves a description nobody can
 * use again, and again under it, since an attachment can publish itself
 * while this call waits for the lock. Only a write that moved bytes disarms
 * the set-id bits, through the node as the write left it (a grow relocates a
 * KRFFS file and repoints every holder), and a privileged writer is exempt:
 * that is the CAP_FSETID analogue, without which any root process disarms
 * /bin/su by rewriting it. A file whose pages shared mappings hold in memory
 * has the bytes written copied into them under the mount's namespace lock,
 * held from before the write, so that no write-back of the pages comes
 * between the two (`filemap_file_written`).
 */
static int32_t file_write(fd_entry_t *entry, const char __user *buf, uint32_t count,
                          const uint64_t *at)
{
    struct open_file *ofd = entry->ofd;
    uint64_t pos;
    int32_t written;
    int mapped;

    if (!(entry->flags & FD_FLAG_WRITE)) {
        return -EBADF;
    }
    if (!ofd || !ofd->vmnt) {
        return -EBADF;
    }
    if (!user_buffer_ok((uint32_t)(__force uintptr_t)buf, count, 0)) {
        return -EFAULT;
    }
    if (loop_backs(ofd->vmnt, ofd->vnode)) {
        return -EBUSY;
    }
    fd_ofd_lock(ofd);
    if (loop_backs(ofd->vmnt, ofd->vnode)) {
        fd_ofd_unlock(ofd);
        return -EBUSY;
    }
    pos = at ? *at : ofd->offset;
    if (ofd->status_flags & OFD_APPEND) {
        struct vfs_stat vst;

        if (vfs_getattr(ofd->vmnt, ofd->vnode, &vst) == 0) {
            pos = vst.size;
        }
    }
    mapped = filemap_disk_mapped(ofd->vmnt, ofd->ino);
    if (mapped) {
        vfs_ns_lock(ofd->vmnt);
    }
    written = vfs_write(ofd->vmnt, ofd->fhandle, (const char __force *)buf, count, pos);
    if (written > 0 && mapped) {
        filemap_file_written(entry, pos, (uint32_t)written);
    }
    if (mapped) {
        vfs_ns_unlock(ofd->vmnt);
    }
    if (written > 0) {
        if (!cred_privileged()) {
            if (entry->node) {
                cred_kill_setid(entry->node);
            } else {
                vfs_kill_setid(ofd->vmnt, ofd->vnode);
            }
        }
        if (!at) {
            ofd->offset = pos + (uint64_t)written;
        }
    }
    fd_ofd_unlock(ofd);
    if (written > 0 && inotify_watches) {
        inotify_desc(ofd, IN_MODIFY);
    }
    return written;
}

/**
 * Write `count` bytes of user buffer `buf` to descriptor `fd`.
 *
 * The shared body of write(2) and writev(2). Dispatches by backing kind: an
 * ops-backed object (pipe/tty/pty/device) through its vtable, a socket to the
 * network, the console through the tty output processor, a KRFFS file, or the
 * framebuffer. Returns the bytes written or a negative errno.
 */
static int32_t write_fd(int fd, const char __user *buf, uint32_t count)
{
    fd_entry_t *entry;

    if (!current_proc) {
        return -ESRCH;
    }

    entry = fd_get(current_proc->fds, fd);
    if (!entry) {
        return -EBADF;
    }

    /* Backing-ops object (pipe/tty/pty/device): dispatch through its vtable,
     * but only if the descriptor was opened for writing (a read-only fd ->
     * -EBADF, as Linux reports). The plain file and enum types below have no
     * ops and fall through to the enum dispatch. */
    if (entry->ops) {
        return write_ops(entry, buf, count);
    }

    if (entry->type == FD_TYPE_SOCKET) {
        if (!entry->sock) {
            return -EBADF;
        }
        if (entry->sock->type == SOCK_STREAM) {
            if (!user_buffer_ok((uint32_t)buf, count, 0)) {
                return -EFAULT;
            }
            /* socket_tcp_send copies the user buffer into the socket send ring
             * itself, blocking on send-ring space unless the fd is
             * non-blocking; __force strips the __user tag at this boundary. */
            return socket_tcp_send(entry->sock, (const uint8_t __force *)buf, (int)count,
                                   entry->ofd && (entry->ofd->status_flags & OFD_NONBLOCK));
        }
        /* A datagram socket writes to the peer it connected to, and has
         * nowhere to send without one. */
        if (entry->sock->type == SOCK_DGRAM) {
            if (!entry->sock->connected) {
                return -EDESTADDRREQ;
            }
            if (!user_buffer_ok((uint32_t)buf, count, 0)) {
                return -EFAULT;
            }
            if (count > 1500) {
                return -EMSGSIZE;
            }
            return dgram_send(entry->sock, entry->sock->remote_ip, entry->sock->remote_port,
                              (const uint8_t __force *)buf, (uint16_t)count);
        }
        return -EOPNOTSUPP;
    }

    if (entry->type == FD_TYPE_CONSOLE) {
        char kbuf[256];
        uint32_t done = 0;

        if (!(entry->flags & FD_FLAG_WRITE)) {
            return -EBADF; /* Not writable (e.g., stdin) */
        }

        /* A background process writing its controlling terminal is stopped with
         * SIGTTOU only when TOSTOP is set (otherwise background output is
         * allowed). As with SIGTTIN, a blocked/ignored SIGTTOU makes it -EIO
         * instead. */
        if ((console_tty.ldisc.tio.c_lflag & TOSTOP) &&
            tty_is_background(&console_tty, (int)current_proc->sid, (int)current_proc->pgid)) {
            if ((current_proc->sig_blocked & KSIG_BIT(SIGTTOU)) ||
                current_proc->sigacts[SIGTTOU].handler == KSIG_IGN) {
                return -EIO;
            }
            tty_signal_group((int)current_proc->pgid, SIGTTOU);
            return -EINTR;
        }

        /* Pull the user bytes in through a small kernel bounce buffer, then
         * emit from kernel memory through the tty output processor
         * (OPOST/ONLCR, so a bare '\n' becomes CR LF on both the VGA screen and
         * the serial line), looping until count bytes are done. A bad user
         * pointer yields -EFAULT only if nothing has been written yet
         * (otherwise report the bytes already emitted, matching the
         * return-count convention). */
        while (done < count) {
            uint32_t chunk = count - done;

            if (chunk > sizeof(kbuf)) {
                chunk = sizeof(kbuf);
            }
            if (copy_from_user(kbuf, (const void __user *)(buf + done), chunk) != 0) {
                return (done > 0) ? (int32_t)done : -EFAULT;
            }
            tty_write(&console_tty, (const unsigned char *)kbuf, (int)chunk);
            done += chunk;
        }
        return (int32_t)count;
    }

    if (entry->type == FD_TYPE_FILE) {
        return file_write(entry, buf, count, NULL);
    }

    if (entry->type == FD_TYPE_FRAMEBUFFER) {
        uint32_t to_copy;

        if (!vga_graphics_mode) {
            return -EBADF; /* Not in graphics mode */
        }

        to_copy = count;
        if (to_copy > VGA_MODE13H_SIZE) {
            to_copy = VGA_MODE13H_SIZE;
        }

        if (copy_from_user((void *)VGA_FRAMEBUFFER, (const void __user *)buf, to_copy) != 0) {
            return -EFAULT;
        }

        return (int32_t)to_copy;
    }

    return -EBADF;
}

/**
 * Truncate an open file to `length` (ftruncate(2)).
 *
 * The descriptor must be a writable file on a mounted file system, and an
 * O_PATH one is -EBADF, as on Linux. A successful shrink or extend clears
 * set-uid, and set-gid where group execute is set, for an unprivileged
 * caller, like a write. `SYS_FTRUNCATE` takes the length in ECX, and
 * `SYS_FTRUNCATE64`, the form musl calls, in ECX and EDX as the low and
 * high halves; the file system answers a length past what it can hold.
 */
static int32_t truncate_fd(int fd, uint64_t length)
{
    fd_entry_t *entry;
    int rc;

    if (!current_proc) {
        return -ESRCH;
    }
    entry = fd_get(current_proc->fds, fd);
    if (!entry || entry->type != FD_TYPE_FILE || (entry->flags & FD_FLAG_PATH)) {
        return -EBADF;
    }
    if (!(entry->flags & FD_FLAG_WRITE)) {
        return -EINVAL; /* ftruncate wants a writable descriptor */
    }
    if (!entry->ofd || !entry->ofd->vmnt) {
        return -EBADF;
    }
    /* Before the description is locked: a return from under that lock is a
     * description nobody can use again. */
    if (loop_backs(entry->ofd->vmnt, entry->ofd->vnode)) {
        return -EBUSY;
    }
    fd_ofd_lock(entry->ofd);
    if (loop_backs(entry->ofd->vmnt, entry->ofd->vnode)) {
        fd_ofd_unlock(entry->ofd); /* attached while this waited for the lock */
        return -EBUSY;
    }
    rc = vfs_truncate(entry->ofd->vmnt, entry->ofd->fhandle, length);
    if (rc == 0 && !cred_privileged()) {
        if (entry->node) {
            cred_kill_setid(entry->node);
        } else {
            vfs_kill_setid(entry->ofd->vmnt, entry->ofd->vnode);
        }
    }
    fd_ofd_unlock(entry->ofd);
    if (rc == 0) {
        filemap_file_truncated(entry->ofd->vmnt, entry->ofd->ino, length);
        if (inotify_watches) {
            inotify_desc(entry->ofd, IN_MODIFY);
        }
    }
    return rc;
}

int32_t sys_ftruncate(isr_params_t *params)
{
    return truncate_fd((int)params->ebx, (uint64_t)params->ecx);
}

int32_t sys_ftruncate64(isr_params_t *params)
{
    return truncate_fd((int)params->ebx, (uint64_t)params->ecx | ((uint64_t)params->edx << 32));
}

/* write(2): EBX = fd, ECX = buf, EDX = count. */
int32_t sys_write(isr_params_t *params)
{
    return write_fd((int)params->ebx, (const char __user *)params->ecx, params->edx);
}

/**
 * Before a writev to `fd` open for writing on a tmpfs file: take the
 * mount's namespace lock, which every write and size change of the file
 * takes, to hold across the elements, and refuse with EPERM, with nothing
 * written, a vector whose elements together would pass the end of a file
 * sealed against growth, as F_SEAL_GROW refuses one write that would
 * (tmpfs.h). The elements counted are those the writes would reach, up to
 * the first the loop refuses, and their bytes are faulted in before the lock
 * is taken, as one write's are. `*held` is the mount to unlock after the
 * elements, or NULL for a descriptor of anything else.
 */
static int32_t writev_begin(int fd, uint32_t iov_user, int iovcnt, struct vfs_mount **held)
{
    fd_entry_t *entry = fd_get(current_proc->fds, fd);
    struct kernel_iovec iov;
    struct open_file *ofd;
    struct vfs_stat st;
    uint32_t extent = 0;
    uint64_t pos;
    int i;

    *held = NULL;
    if (!entry || entry->type != FD_TYPE_FILE || !(entry->flags & FD_FLAG_WRITE) || !entry->ofd ||
        !entry->ofd->vmnt || entry->ofd->vmnt->type != &vfs_tmpfs_type) {
        return 0;
    }
    ofd = entry->ofd;
    for (i = 0; i < iovcnt; i++) {
        if (copy_from_user(
                &iov, (const void __user *)(iov_user + (uint32_t)i * sizeof(struct kernel_iovec)),
                sizeof(iov)) != 0 ||
            iov.iov_len > 0x7FFFFFFFu || extent > 0x7FFFFFFFu - iov.iov_len ||
            (iov.iov_len > 0 && !user_buffer_ok(iov.iov_base, iov.iov_len, 0))) {
            break;
        }
        extent += iov.iov_len;
    }
    vfs_ns_lock(ofd->vmnt);
    *held = ofd->vmnt;
    if (extent == 0 || !(tmpfs_seals(ofd->vnode) & TMPFS_SEAL_GROW) ||
        vfs_getattr(ofd->vmnt, ofd->vnode, &st) != 0) {
        return 0;
    }
    pos = (ofd->status_flags & OFD_APPEND) ? st.size : ofd->offset;
    if (pos + extent > st.size) {
        vfs_ns_unlock(ofd->vmnt);
        *held = NULL;
        return -EPERM;
    }
    return 0;
}

/**
 * Write the `iovcnt` elements of the vector at `iov_user` to `fd`, one at a
 * time, and return the bytes written. A fault, a length Linux refuses or a
 * failed write ends the call: with the error when nothing was written yet,
 * and otherwise with the bytes before it, as a short write does.
 */
static int32_t writev_elements(int fd, uint32_t iov_user, int iovcnt)
{
    struct kernel_iovec iov;
    int32_t total = 0;
    int i;

    for (i = 0; i < iovcnt; i++) {
        int32_t r;

        /* On any fault once bytes were already emitted, report the partial
         * total (a caller that retried would otherwise duplicate output); only
         * report the error itself when nothing has been written yet. */
        if (copy_from_user(
                &iov, (const void __user *)(iov_user + (uint32_t)i * sizeof(struct kernel_iovec)),
                sizeof(iov)) != 0) {
            return (total > 0) ? total : -EFAULT;
        }
        if (iov.iov_len == 0) {
            continue;
        }
        /* An individual length past the signed range, or a running total that
         * would overflow it, is rejected as Linux does (-EINVAL). */
        if (iov.iov_len > 0x7FFFFFFFu || (uint32_t)total > 0x7FFFFFFFu - iov.iov_len) {
            return (total > 0) ? total : -EINVAL;
        }
        if (!user_access_ok(iov.iov_base, iov.iov_len)) {
            return (total > 0) ? total : -EFAULT;
        }

        r = write_fd(fd, (const char __user *)iov.iov_base, iov.iov_len);
        if (r < 0) {
            return (total > 0) ? total : r;
        }
        total += r;
        if ((uint32_t)r < iov.iov_len) {
            break; /* short write; stop, as writev does */
        }
    }

    return total;
}

/**
 * writev(2): EBX = fd, ECX = the vector, EDX = its count. The elements are
 * written in order (`writev_elements`); on a tmpfs file the mount's lock is
 * held across them, and a vector a seal refuses whole is refused before the
 * first (`writev_begin`).
 */
int32_t sys_writev(isr_params_t *params)
{
    int fd = (int)params->ebx;
    uint32_t iov_user = params->ecx;
    int iovcnt = (int)params->edx;
    struct vfs_mount *held;
    int32_t rc;

    if (iovcnt < 0 || iovcnt > IOV_MAX_LOCAL) {
        return -EINVAL;
    }
    if (!current_proc) {
        return -ESRCH;
    }
    rc = writev_begin(fd, iov_user, iovcnt, &held);
    if (rc == 0) {
        rc = writev_elements(fd, iov_user, iovcnt);
    }
    if (held) {
        vfs_ns_unlock(held);
    }
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
 * `base` is "/" or "/a/b" with no trailing slash, and not inside `buf`.
 * Returns the new length, or -ENAMETOOLONG when the whole does not fit `size`.
 */
static int path_prepend(char *buf, uint32_t size, uint32_t len, const char *base)
{
    uint32_t blen = (uint32_t)k_strlen(base);

    if (blen <= 1) {
        blen = 0; /* the root: the slash alone */
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
    int out = 1; /* path[0] is '/'; out is the current length */
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
        /* `..` stays in the path: it is resolved physically by the walk,
         * where a missing or unsearchable directory before it fails, so a
         * spelling can no longer reach past what the caller may see. */
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
 * Re-validate a descriptor's cached KRFFS node before a file operation.
 *
 * A held node is never freed now, so this stops being an identity test and
 * becomes what it always was in fact, a resolution. Two shapes are live: an
 * orphan, which no path names and which nothing will free while this descriptor
 * holds it; and a named node, which must still resolve back to itself. Anything
 * else is a descriptor whose file went away by a route that did not consult the
 * holders, and stays -EBADF.
 *
 * The old round-trip could not tell a freed node from a different file the
 * allocator later placed at the same address. That hole is closed for every
 * regular-file descriptor by the lifetime, not by this check: no address
 * a descriptor has is handed out while it has it.
 *
 * A NULL node is the root directory and needs no check. A directory is
 * checked the same way, though `rmdir` refuses one a description has: one
 * that fails was removed by a route that did not look for its descriptors.
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
        return 0; /* an orphan: unreachable by name, live for its holders */
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
 * Make `path`, a path from the root of the mount table, the path the calling
 * process has for the same place from the root chroot gave it: the root's
 * own path comes off the front, and the root itself is "/". A process with
 * no root of its own keeps `path` as it is.
 *
 * The root is held while its path is rebuilt from its volume, which may
 * sleep, as `cwd_global` holds it: a thread that shares the directories
 * (CLONE_FS) may chroot meanwhile and let the old root go. Returns 0;
 * -ENOENT for a place outside the process's root, which no path of the
 * process reaches; -ENOMEM; -EBUSY for a root on a mount being unmounted; or
 * the error `vfs_node_path` gives for the root.
 */
static int32_t path_in_root(char *path)
{
    struct vfs_ref r;
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
    r.mnt = current_proc->fs->root_mnt;
    r.node = current_proc->fs->root_node;
    if (vfs_enter(r.mnt) != 0) {
        path_buf_put(root);
        return -EBUSY;
    }
    vfs_ref_hold(&r);
    rc = vfs_node_path(r.mnt, r.node, root, VFS_PATH_MAX);
    vfs_ref_release(&r);
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
 * The absolute path of the directory a descriptor is open on, into `out` of
 * `cap` bytes: what its `*at` calls resolve relative names against, and what
 * `fchdir` makes the working directory.
 *
 * A synthetic directory's path is rebuilt from its kind and pid. A directory
 * of a mounted type is named by the type, so the descriptor follows a rename
 * of it, behind the path of the directory the mount covers; a type without a
 * `path` operation keeps the path the directory was opened by. A directory of
 * the KRFFS boot root is named by its node, which must still resolve back to
 * itself: KRFFS nodes are not counted, and though `rmdir` refuses a directory
 * a description has, the test does not rest on that. A path from the root of
 * the mount table becomes the one the calling process has from its own root
 * (`path_in_root`). Returns 0, -ENOTDIR for a descriptor on anything but a
 * directory, -EBADF for a boot-root directory that no longer resolves to its
 * node, -ENOENT for one outside the process's root or one tmpfs removed, or
 * -ENAMETOOLONG.
 */
static int fd_dir_path(fd_entry_t *entry, char *out, uint32_t cap)
{
    krffs_node_t *dnode = entry->node; /* NULL for the root, and for other types */
    int r;

    if (entry->type == FD_TYPE_SYNTH_DIR) {
        struct synth_dir *sd = (struct synth_dir *)entry->priv;

        if (!sd) {
            return -EBADF;
        }
        synth_dir_path(sd->kind, sd->pid, sd->tid, out, (int)cap);
        return path_in_root(out);
    }
    if (entry->type != FD_TYPE_DIR) {
        return -ENOTDIR;
    }
    if (!dnode && entry->ofd && entry->ofd->vmnt && !mnt_is_root(entry->ofd->vmnt)) {
        struct vfs_mount *vm = entry->ofd->vmnt;

        if (vm->type->ops->path) {
            r = vfs_node_path(vm, entry->ofd->vnode, out, cap);
            return r != 0 ? r : path_in_root(out);
        }
        if (entry->ofd->dirpath[0] == '\0' || k_strlen(entry->ofd->dirpath) + 1 > cap) {
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
        return path_in_root(out);
    }
    out[0] = '/';
    out[1] = '\0';
    return path_in_root(out);
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
 * one does not read `dirfd`, -EBADF for a descriptor that is not open and
 * -ENOTDIR for one that is not a directory.
 */
static int copy_path_at(char *pathbuf, uint32_t size, int dirfd, const char __user *upath, int *dir)
{
    fd_entry_t *entry;
    char *dirpath;
    int len, r = 0;

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
        r = fd_dir_path(entry, dirpath, VFS_PATH_MAX);
        if (r == 0) {
            r = path_prepend(pathbuf, size, (uint32_t)len, dirpath);
        }
        path_buf_put(dirpath);
    }
    if (r < 0) {
        return r;
    }
    len = canonicalize_abs(pathbuf);
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

/** `path_from_user_dir` for a caller that does not ask how the path ends. */
static int path_from_user(int dirfd, const char __user *upath, char **out)
{
    return path_from_user_dir(dirfd, upath, out, NULL);
}

/**
 * The path of execve(2), copied into `pathbuf` of `size` bytes as
 * `copy_path_at` copies it for `AT_FDCWD`, for the program it runs, with
 * `*end` saying how it named a directory by its end and a last "." put back.
 * Such a path must reach a directory, which no program is, so the one walk
 * that finds the program answers for the end (`resolve_named`): a file so
 * named is -ENOTDIR and a directory the open's -EACCES, as on Linux. Returns
 * the length, or a negative errno.
 */
int copy_path_cwd(char *pathbuf, uint32_t size, const char __user *upath, int *end)
{
    return copy_path_at(pathbuf, size, AT_FDCWD, upath, end);
}

/**
 * Make the absolute path `path`, which the kernel read from a file, canonical
 * in place, as `copy_path_cwd` makes execve's, with `*end` saying how it
 * named a directory by its end and a last "." put back: the interpreter a
 * script names, or an ELF image's PT_INTERP, whose one walk answers for the
 * end as the program's does. Returns the length.
 */
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

/**
 * Record `name`, the path a description was opened by with its leading '/'
 * stripped, in `ofd`, for /proc/PID/fd to show and for a directory of a type
 * without `path` to resolve names against: without the last "." that
 * `path_put_dot` put back, and empty when the path is too long for the
 * record.
 */
static void ofd_record_path(struct open_file *ofd, const char *name)
{
    uint32_t len = (uint32_t)k_strlen(name);

    if (len > 0 && name[len - 1] == '.' && (len == 1 || name[len - 2] == '/')) {
        len = len > 1 ? len - 2 : 0;
    }
    ofd->dirpath[0] = '\0';
    if (len + 2 <= sizeof(ofd->dirpath)) {
        ofd->dirpath[0] = '/';
        k_memcpy(ofd->dirpath + 1, name, len);
        ofd->dirpath[len + 1] = '\0';
    }
}

static int32_t synth_open_dir(const struct vpath *vp, int flags);

static int32_t open_path(const char *name, int flags);

static int32_t open_named(const char *name, int flags, uint32_t mode, int end);

/** Non-zero when the caller has appropriate privilege (`proc_privileged`). */
int cred_privileged(void)
{
    return proc_privileged(current_proc);
}

/**
 * Drop the set-id bits a modification must not leave standing on `node`.
 *
 * Setuid always goes: a file whose contents or owner just changed no longer
 * grants what its owner meant it to grant. Setgid goes only when the
 * group-execute bit is set with it, matching the grant rule in `execve`:
 * without that bit the file cannot hand out a gid, and the pair means the
 * mandatory-lock mark elsewhere in Unix.
 *
 * The root node returns untouched, and by node identity, not by
 * `KRFFS_IS_DIR`: `mkfs` stamps the root a bare 0755 with no type bits, so
 * the directory test reads false on it, and `KRFFS_MODE_DIRTY` shares the
 * setuid position. A clear that reached the root would report an unclean
 * image as cleanly unmounted, which is file-system corruption, not a
 * permission bug, and `chown(path, -1, -1)` is permitted to every caller.
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

/**
 * The same rule for a file of another type, through its attributes: a
 * write or a truncation by a caller without the privilege drops the set-id
 * bits, so that a program a group may modify cannot keep root's identity.
 * A directory keeps its bits, as on Linux.
 */
void vfs_kill_setid(struct vfs_mount *mnt, void *node)
{
    struct vfs_stat vst;

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
    (void)vfs_setattr(mnt, node, VFS_SET_MODE, &vst);
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

/** Non-zero when `gid` is the caller's effective gid or in its supplementary
 * list: the set a file owner may chgrp to, and the group the access check
 * matches. */
static int cred_in_group(uint32_t gid)
{
    return gid == current_proc->egid || cred_in_suppl(gid);
}

/**
 * The nine-bit permission check over explicit metadata: one identity class is
 * chosen and judged alone, the classic rule. The owner is judged by the owner
 * bits, else a group match (primary or supplementary) by the group bits, else
 * the other bits; an effective uid of 0 passes everything, unless the thread
 * gave up its capabilities (`nocaps`). `use_real` judges by the real ids
 * instead, for access(2), the one call that asks who the process really is,
 * not who it is acting as.
 */
static int32_t cred_access_bits(uint32_t owner, uint32_t group, uint32_t mode, int want,
                                int use_real)
{
    process_t *p = current_proc;
    uint32_t uid = use_real ? p->ruid : p->euid;
    uint32_t bits;

    if (uid == 0 && !p->nocaps) {
        return 0;
    }
    if (uid == owner) {
        bits = mode >> 6;
    } else if ((use_real ? p->rgid : p->egid) == group || cred_in_suppl(group)) {
        bits = mode >> 3;
    } else {
        bits = mode;
    }
    return ((uint32_t)want & ~bits & 7u) ? -EACCES : 0;
}

/* Whether `mnt` is the KRFFS root, whose nodes take the KRFFS-specific
 * paths below; an ext2 root is entry zero as well and goes the way of every
 * other type. */
int mnt_is_root(const struct vfs_mount *mnt)
{
    return mnt == vfs_root_mount() && mnt->type == &vfs_krffs_type;
}

/**
 * The permission check over a node of any type, from the attributes its
 * file system reports: the same nine-bit rule as cred_access_node, plus
 * the execute rule for a regular file.
 */
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
    return cred_access_bits(st.uid, st.gid, st.mode & 0777u, want, use_real);
}

/**
 * The permission check over a KRFFS node. The root directory reads as 0755,
 * the fixed identity stat reports (its stored mode has the dirty-mount
 * flag). Executing a regular file demands an execute bit somewhere even for
 * root: root runs programs, not data.
 */
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
    if (cred_privileged() || current_proc->euid == dir->uid ||
        (victim && current_proc->euid == victim->uid)) {
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
 * for a description made by O_PATH. The description is a new one.
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
    rc = open_install(&ref, fd, src->dirpath[0] == '/' ? src->dirpath + 1 : "", flags);
    vfs_walk_unlock(mnt);
    return rc;
}

/**
 * A new descriptor that shares the description of descriptor `procfd` of
 * the table `fds`, as dup(2) shares it, close-on-exec where `flags` has
 * O_CLOEXEC. The source is looked up again once the slot is had, since the
 * caller's table may have grown; nothing between the caller's checks and the
 * copy sleeps, the slot's memory coming from the heap, which does not wait.
 * Returns the descriptor or a negative errno.
 */
static int32_t proc_fd_share(fd_table_t *fds, int procfd, int flags)
{
    fd_entry_t *src;
    fd_entry_t *dst;
    int fd = fd_alloc_from(current_proc->fds, 0);

    if (fd < 0) {
        return fd;
    }
    src = &fds->entries[procfd];
    dst = &current_proc->fds->entries[fd];
    fd_ofd_unref(dst->ofd);
    dst->type = src->type;
    dst->node = src->node;
    dst->sock = src->sock;
    dst->ops = src->ops;
    dst->priv = src->priv;
    dst->dev = src->dev;
    dst->ofd = src->ofd;
    fd_ofd_ref(dst->ofd);
    dst->flags =
        (src->flags & ~(uint32_t)FD_FLAG_CLOEXEC) | ((flags & O_CLOEXEC) ? FD_FLAG_CLOEXEC : 0u);
    if (src->type == FD_TYPE_SOCKET && src->sock) {
        socket_ref(src->sock);
    }
    return fd;
}

/**
 * open(2) of /proc/PID/fd/N, which /dev/fd/N, /dev/stdin, /dev/stdout and
 * /dev/stderr are for the caller, following the link to what the descriptor
 * is open on, as Linux does. A file or directory is opened again
 * (`proc_fd_reopen`), with its description held across the sleeps of that
 * open, since another thread may close the descriptor meanwhile. Anything
 * else, a pipe, a socket, a terminal or a device, has the descriptor's own
 * description shared with the new one (`proc_fd_share`), when the
 * descriptor has the access asked for and is no O_DIRECTORY open of what is
 * not a directory; Linux opens a pipe's other end for the other access, and
 * this answers EACCES. Another process's descriptor needs a privileged
 * caller or one with the process's effective user ID.
 */
static int32_t proc_fd_open(const struct vpath *vp, int flags)
{
    int pid = procfs_resolve_id(vp);
    process_t *p = pid < 0 ? NULL : get_process((uint32_t)pid);
    unsigned acc = (unsigned)flags & 3u;
    struct open_file *held;
    fd_entry_t *src;
    int32_t rc;

    if (!p || p->state == PROC_UNUSED || !p->fds || vp->procfd < 0 ||
        vp->procfd >= fd_table_cap(p->fds)) {
        return -ENOENT;
    }
    if (p->fds != current_proc->fds && !cred_privileged() && p->euid != current_proc->euid) {
        return -EACCES;
    }
    src = &p->fds->entries[vp->procfd];
    if (src->type == FD_TYPE_NONE || (src->flags & FD_FLAG_CLOSING)) {
        return -ENOENT;
    }
    if (src->ofd && src->ofd->vmnt && (src->type == FD_TYPE_FILE || src->type == FD_TYPE_DIR)) {
        held = src->ofd;
        fd_ofd_ref(held);
        rc = proc_fd_reopen(held, flags);
        fd_ofd_unref(held);
        return rc;
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

/**
 * Open a path that classified as synthetic (/dev or /proc).
 *
 * Devices bind their file_ops; directories get a synthetic dir fd; /proc files
 * snapshot their text, but for oom_score_adj and mem, which reach the process
 * at each read. A synthetic mount is never created or fallen through to
 * KRFFS.
 */
static int32_t synth_open(const struct vpath *vp, int flags)
{
    switch (vp->kind) {
    case VNODE_DEV: {
        /* The nine-bit check devices always deserved and only bootpart
         * needs: every entry is owned root:root, so for the 0666 devices
         * this grants what it always granted, and the raw boot-partition
         * window (0600) opens for root alone. */
        uint32_t dmode;
        int want;

        if (devfs_stat(vp->name, &dmode, NULL, NULL) != 0) {
            return -ENOENT;
        }
        switch (flags & 3) {
        case 1:
            want = CRED_W;
            break;
        case 2:
            want = CRED_R | CRED_W;
            break;
        default:
            want = CRED_R;
            break;
        }
        {
            int32_t rc = cred_access_bits(0, 0, dmode & 0777u, want, 0);
            if (rc != 0) {
                return rc;
            }
        }
        return devfs_open(vp->name, flags);
    }
    case VNODE_PTS:
        /* /dev/pts/N: the slave end of pty N (pty_open_slave does its own
         * fd allocation, and rejects an unallocated/locked pair). The open
         * is permission-checked against the pty's ownership, so one user
         * cannot open another's terminal. */
        if (flags & O_DIRECTORY) {
            return -ENOTDIR;
        }
        {
            struct pty *pt = pty_get(vp->pts_index);
            if (pt) {
                unsigned acc = (unsigned)flags & 3u;
                int want = (acc == 0) ? CRED_R : (acc == 1) ? CRED_W : (CRED_R | CRED_W);
                int32_t rc =
                    cred_access_bits(pt->uid, pt->gid, (uint32_t)(pt->mode & 0777u), want, 0);
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
    case VNODE_PROC_SYS_FILE:
    case VNODE_PROC_MOUNTS:
    case VNODE_PROC_LOOP:
    case VNODE_PROC_AUDIO:
    case VNODE_PROC_USB:
    case VNODE_PROC_FILESYSTEMS:
    case VNODE_PROC_INTERRUPTS:
    case VNODE_PROC_CONSOLE:
    case VNODE_PROC_NET_ROUTE:
    case VNODE_PROC_NET_ARP:
    case VNODE_PROC_NET_TCP:
    case VNODE_PROC_NET_FILE:
    case VNODE_SYS_FILE:
        return procfs_open(vp, flags);
    case VNODE_PROC_DISK:
        /* Root only. The file has the drive's serial number, and an
         * image that has this file also has an unprivileged account. */
        {
            int32_t rc = cred_access_bits(0, 0, 0400u, CRED_R, 0);
            if (rc != 0) {
                return rc;
            }
        }
        return procfs_open(vp, flags);
    case VNODE_DEV_DIR:
    case VNODE_PTS_DIR:
    case VNODE_PROC_DIR:
    case VNODE_PROC_NET_DIR:
    case VNODE_PROC_SYS_DIR:
    case VNODE_PROC_PID:
    case VNODE_PROC_FD_DIR:
    case VNODE_PROC_TASK_DIR:
    case VNODE_SYS_DIR:
    case VNODE_SYS_LINK:
        return synth_open_dir(vp, flags);
    case VNODE_ENOTDIR:
        return -ENOTDIR;
    case VNODE_PROC_FD_LINK:
        return proc_fd_open(vp, flags);
    default:
        return -ENOENT;
    }
}

/* Open a synthetic directory (/dev, /proc, /proc/<pid>, /proc/<pid>/fd, and
 * a thread's under /proc/<pid>/task). The descriptor keeps a record of its
 * own, taken from the heap and freed at the last close, with the process's
 * pid and, for a thread's directory, the thread's id, so its listing, its
 * /proc/PID/fd link and the names relative to it stay the thread's. Another
 * user's /proc/PID/fd is EACCES, as Linux keeps a process's descriptors to
 * those who may trace it (`procfs_may_read`). */
static int32_t synth_open_dir(const struct vpath *vp, int flags)
{
    fd_entry_t *e;
    int fd;
    int pid = 0;

    struct synth_dir *sd;

    if (vp->kind == VNODE_PROC_PID || vp->kind == VNODE_PROC_FD_DIR ||
        vp->kind == VNODE_PROC_TASK_DIR) {
        if (procfs_resolve_id(vp) < 0) {
            return -ENOENT;
        }
        pid = procfs_resolve_pid(vp->pid);
    }
    if (vp->kind == VNODE_PROC_FD_DIR && !procfs_may_read(pid)) {
        return -EACCES;
    }
    /* The directory exists, so honour flags that contradict that. An exclusive
     * create of an existing object is -EEXIST regardless of access mode (the
     * usual O_CREAT|O_EXCL form implies write access), so that check comes
     * first; then a directory cannot be opened for writing, nor by O_CREAT,
     * which asks for a file. */
    if ((flags & O_CREAT) && (flags & O_EXCL)) {
        return -EEXIST;
    }
    if ((flags & 3) != 0 || (flags & O_CREAT)) {
        return -EISDIR;
    }
    if ((vp->kind == VNODE_SYS_DIR || vp->kind == VNODE_SYS_LINK) && SYSFS_KEYED(vp->sysent) &&
        !sysfs_device_exists(vp->sysent, vp->syskey)) {
        return -ENOENT;
    }
    if (vp->kind == VNODE_SYS_DIR || vp->kind == VNODE_SYS_LINK) {
        sd = synth_dir_alloc(VNODE_SYS_DIR, vp->sysent, vp->syskey);
    } else {
        sd = synth_dir_alloc(vp->kind, vp->kind == VNODE_PROC_SYS_DIR ? vp->sysent : pid, vp->tid);
    }
    if (!sd) {
        return -ENFILE;
    }
    fd = fd_alloc(current_proc->fds);
    if (fd < 0) {
        kfree(sd);
        return fd;
    }
    e = &current_proc->fds->entries[fd];
    e->type = FD_TYPE_SYNTH_DIR;
    e->node = NULL;
    e->sock = NULL;
    e->ops = NULL;
    e->priv = sd;
    e->ofd->offset = 0;
    e->flags = FD_FLAG_READ;
    if (flags & O_CLOEXEC) {
        e->flags |= FD_FLAG_CLOEXEC;
    }
    return fd;
}

/**
 * Stamps a newly created KRFFS node in `parent` with the creating process's
 * identity: owner and group from the effective ids, permission bits from
 * `mode` with the file-creation mask applied, the type bits kept. The V7
 * creation rule: who you effectively are is whose the file is. A setgid
 * parent gives its group instead, and its setgid bit to a new directory, as
 * Linux's `inode_init_owner` does.
 */
static void krffs_stamp_new_node(krffs_node_t *node, const krffs_node_t *parent, uint32_t mode)
{
    uint32_t perm = mode & 07777u & ~current_proc->fs->umask;

    node->uid = current_proc->euid;
    node->gid = current_proc->egid;
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
 * The framebuffer descriptor, for an open of the name "fb". One process at a
 * time owns the framebuffer, by its thread group id, which the ioctl, the
 * console read and the last close compare too, so any of its threads may use
 * it; the open switches the display to mode 13h.
 */
static int32_t open_framebuffer(void)
{
    fd_entry_t *entry;
    int fd;

    if (fb_owner_pid != 0 && fb_owner_pid != current_proc->tgid) {
        return -EBUSY; /* Already in use */
    }

    fd = fd_alloc(current_proc->fds);
    if (fd < 0) {
        return fd;
    }

    entry = &current_proc->fds->entries[fd];
    entry->type = FD_TYPE_FRAMEBUFFER;
    entry->node = NULL;
    entry->ofd->offset = 0;
    entry->flags = FD_FLAG_READ | FD_FLAG_WRITE;

    vga_mode13h_enter();
    fb_owner_pid = current_proc->tgid;

    kbd_clear_scancodes(); /* drop stale scancodes left by the shell */

    return fd;
}

/**
 * The mouse descriptor, for an open of the name "mouse".
 */
static int32_t open_mouse(void)
{
    fd_entry_t *entry;
    int fd;

    if (!mouse_is_available() && !input_mouse_present()) {
        return -ENODEV; /* no mouse of any kind was found */
    }

    fd = fd_alloc(current_proc->fds);
    if (fd < 0) {
        return fd;
    }

    entry = &current_proc->fds->entries[fd];
    entry->type = FD_TYPE_MOUSE;
    entry->node = NULL;
    entry->ofd->offset = 0;
    entry->flags = FD_FLAG_READ;

    mouse_clear_packets();

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
 * the mouse is touched. The other flags are ignored.
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
        *result = fb ? open_framebuffer() : open_mouse();
    }
    return 1;
}

/**
 * The target of a symbolic link at `name` whose own target is missing, for
 * O_CREAT to create: as on Linux, the target is what is created. Returns 1
 * with the target's path in `through`, the link's directory in front of a
 * relative one; 0 when `name` is not a link; or a negative errno, ELOOP once
 * `followed` passes the resolver's bound of forty links. `name` may be in
 * `through`, from the link before.
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
        tlen = -ENOENT; /* an empty target names nothing, as the resolver answers */
    }
    if (tlen >= 0 && ++*followed > VFS_LINK_MAX) {
        tlen = -ELOOP;
    }
    if (tlen >= 0 && target[0] != '/') {
        /* The link's own directory, then the target. */
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

/**
 * The missing-file half of O_CREAT: hold the parent of `name`, check its
 * write bit, and make the node under the namespace lock with `node_make`,
 * of the kind the type bits of `mode` give, which an open passes none of.
 * Returns 0 with `ref` on the new node, held by its creation; 1 when another
 * creator held the lock first, so the caller opens what it made; or a
 * negative errno. On a type that sets `ns_serialised`, success retains the
 * namespace lock through open-handle registration; another type's lock is
 * not taken.
 */
static int open_create(const char *name, int flags, uint32_t mode, struct vfs_ref *ref)
{
    struct vfs_ref parent;
    char leafname[VFS_NAME_MAX + 1];
    void *made;
    int held;
    int rc;

    rc = vfs_resolve(0, name, VFS_WALK_PARENT | VFS_WALK_LOCK, &parent, leafname, sizeof(leafname));
    if (rc != 0) {
        return rc;
    }
    /* The walk locked the mount only if its type asks to be serialised. The
     * create runs under the lock on every type, as it always has, so a type
     * the walk left alone takes it here and gives it back at once. */
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
        vfs_ns_lock(parent.mnt);
    }
    rc = node_make(&parent, leafname, mode, 0, &made);
    if (!held) {
        vfs_ns_unlock(parent.mnt);
    }
    if (rc == -EEXIST && !(flags & O_EXCL)) {
        /* Another creator held the lock first: the caller opens what it
         * made. */
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
    ref->node = made; /* held by creation; the parent's hold goes back */
    if (parent.mnt->type->ops->put) {
        parent.mnt->type->ops->put(parent.mnt, parent.node);
    }
    return 0;
}

/**
 * The FIFO half of `open_special`: the descriptor `fd` the open reserved,
 * whose description holds the file and its mount, as a regular file's does,
 * made an end of the file's pipe. The walk's lock goes before
 * `fifo_attach`, which may wait for the other end, with the slot still a
 * reservation. Returns the descriptor or a negative errno, with `ref`
 * released and the reservation installed or freed either way.
 */
static int32_t open_fifo(struct vfs_ref *ref, int fd, uint64_t ino, int flags)
{
    struct vfs_mount *mnt = ref->mnt;
    fd_entry_t *e = &current_proc->fds->entries[fd];
    int32_t rc;

    rc = vfs_open_node(mnt, ref->node, flags, &e->ofd->fhandle);
    if (rc != 0) {
        vfs_walk_unlock(mnt);
        vfs_ref_release(ref);
        fd_free(current_proc->fds, fd);
        return rc;
    }
    e->ofd->vmnt = mnt;
    e->ofd->vnode = ref->node;
    e->ofd->ino = ino;
    vfs_desc_open(mnt);
    vfs_walk_unlock(mnt);
    vfs_leave(mnt);
    rc = fifo_attach(e, mnt, ino, flags);
    if (rc != 0) {
        fd_release_orphan(e);
        fd_free(current_proc->fds, fd);
        return rc;
    }
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
 * Open the FIFO, the socket file or the device node at `ref`, which name no
 * bytes of their own: 1 with the descriptor or the errno in `*result`, `ref`
 * and the walk's lock released, and the descriptor `fd` the open reserved
 * taken, by a FIFO, or freed, since a device's open takes a descriptor of
 * its own; 0, with all three as they were, for any other node, which the
 * ordinary open takes.
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
 * share. The node stays where it is, a FIFO having no bytes to move, and
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

    if (mnt_is_root(mnt)) {
        krffs_node_t *node = ref->node;

        if (node == krffs_fs->node) {
            return 0;
        }
        type = krffs_mode_type(node->mode);
        ino = (uint64_t)(uintptr_t)node;
    } else {
        struct vfs_stat st;

        if (vfs_getattr(mnt, ref->node, &st) != 0) {
            return 0; /* the ordinary open reports the error */
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
    vfs_walk_unlock(mnt);
    vfs_ref_release(ref);
    fd_free(current_proc->fds, fd);
    if (rc == 0) {
        rc = type == S_IFSOCK ? -ENXIO : open_device(type == S_IFBLK, rdev, flags);
    }
    *result = rc;
    return 1;
}

/**
 * The checks on an existing node of a mounted type, from its attributes in
 * the order open(2) keeps (EEXIST, EISDIR, ENOTDIR), then the mount's EROFS
 * for a write-side request, then permission and loop attachment. A directory
 * is EISDIR to an open for writing, to O_TRUNC, and to O_CREAT, which asks
 * for a file, as on Linux. Truncation uses the open handle in
 * `open_install`. On an error the reference is released.
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
        return -ELOOP; /* the link itself, under O_NOFOLLOW */
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
 * The same checks on a KRFFS node: O_CREAT|O_EXCL, the directory rules, the
 * permission the access mode asks for, then O_TRUNC, which only ever shrinks
 * and so keeps the node where it is. On an error the reference is released.
 */
static int open_root_node(struct vfs_ref *ref, int flags)
{
    krffs_node_t *node = ref->node;
    int is_dir = (node == krffs_fs->node) || KRFFS_IS_DIR(node->mode);
    unsigned acc = (unsigned)flags & 3u;
    int want;
    int32_t crc;
    int rc;

    /* O_CREAT|O_EXCL requires the file not already exist. */
    if ((flags & O_CREAT) && (flags & O_EXCL)) {
        vfs_ref_release(ref);
        return -EEXIST;
    }
    /* A directory cannot be opened for writing, truncated, or opened by
     * O_CREAT, which asks for a file (the access mode is the low two
     * bits); O_DIRECTORY requires one. */
    if (is_dir && (acc != 0 || (flags & (O_TRUNC | O_CREAT)))) {
        vfs_ref_release(ref);
        return -EISDIR;
    }
    if (!is_dir && (flags & O_DIRECTORY)) {
        vfs_ref_release(ref);
        return -ENOTDIR;
    }
    /* The open itself is permission-checked: the access mode asks for
     * read, write, or both, and truncation is a write. */
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
    if (flags & O_TRUNC) {
        /* A truncate to zero only ever shrinks, and a shrink keeps the
         * node where it is. This branch is already past the -EISDIR for
         * the root and for any directory, so only a regular file
         * reaches it. */
        rc = vfs_truncate(ref->mnt, node, 0);
        if (rc != 0) {
            vfs_ref_release(ref);
            return rc;
        }
        if (!cred_privileged()) {
            cred_kill_setid(node);
        }
    }
    return 0;
}

/**
 * The descriptor for an opened node, in the slot `fd` the open reserved: a
 * directory opens as a FD_TYPE_DIR (its node is the directory whose children
 * getdents lists); a regular file as FD_TYPE_FILE. A path that resolved to
 * the root (e.g. "x/..") is the root directory, stored with a NULL node like
 * a literal "/" open. A node of another type keeps `node` NULL: the
 * description's own fields name it, and the KRFFS-only paths (the holder
 * rule, the set-id clear) have nothing to do with it. Returns the fd or a
 * negative errno, with the reference released and, on an error, the
 * reservation freed. Register the mounted description before truncation so
 * a relocation while waiting updates its node. A failed truncation frees
 * the description and its open handle.
 *
 * The entry gets its type last, which is what publishes it: until then the
 * slot is a reservation, which no other thread's lookup, close or dup2 can
 * take while the file system's open or the truncation sleeps with the
 * description in use.
 */
static int32_t open_install(struct vfs_ref *ref, int fd, const char *name, int flags)
{
    fd_entry_t *entry = &current_proc->fds->entries[fd];
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

    /* The description takes the resolution's hold and one reference on the
     * mount; its last close gives all of it back through vfs_desc_close. */
    rc = vfs_open_node(ref->mnt, node, flags, &entry->ofd->fhandle);
    if (rc != 0) {
        fd_free(current_proc->fds, fd);
        vfs_ref_release(ref);
        return rc;
    }
    entry->ofd->vmnt = ref->mnt;
    entry->ofd->vnode = node;
    vfs_desc_open(ref->mnt);
    vfs_leave(ref->mnt);
    if (!mnt_is_root(ref->mnt) && (flags & O_TRUNC)) {
        rc = vfs_truncate(ref->mnt, entry->ofd->fhandle, 0);
        if (rc == 0 && !cred_privileged()) {
            vfs_kill_setid(ref->mnt, entry->ofd->vnode);
        }
        if (rc != 0) {
            fd_free(current_proc->fds, fd);
            return rc;
        }
    }

    /* Access mode is the low two bits (O_RDONLY=0/O_WRONLY=1/O_RDWR=2); create,
     * trunc and cloexec live higher up, so mask before testing; the legacy
     * `flags == 0` form misreads O_RDONLY|O_CREAT as not-readable. */
    if ((flags & 3) == 1) {
        entry->flags = FD_FLAG_WRITE;
    } else if ((flags & 3) == 2) {
        entry->flags = FD_FLAG_READ | FD_FLAG_WRITE;
    } else {
        entry->flags = FD_FLAG_READ;
    }
    if (entry->ofd) {
        /* O_APPEND/O_NONBLOCK are OFD status flags, shared by dup/fork. */
        if (flags & O_APPEND) {
            entry->ofd->status_flags |= OFD_APPEND;
        }
        if (flags & O_NONBLOCK) {
            entry->ofd->status_flags |= OFD_NONBLOCK;
        }
    }
    if (flags & O_CLOEXEC) {
        entry->flags |= FD_FLAG_CLOEXEC; /* opendir() and fopen(.,"e") rely on this */
    }
    entry->node = mnt_is_root(entry->ofd->vmnt) && entry->ofd->vnode != krffs_fs->node
                      ? (krffs_node_t *)entry->ofd->vnode
                      : NULL;
    entry->type = type;
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
 * Open core shared by open(5) and openat(295).
 *
 * `name` is a kernel-space path with any leading '/' already stripped; `flags`
 * are the Linux open flags and `mode` the permission bits to stamp on a file
 * created by O_CREAT (ignored otherwise). Opens a legacy device ("fb" or
 * "mouse", `open_device_name`) or resolves a path on any mounted type and
 * installs a descriptor. Returns the fd or -errno. Opening "/" opens the
 * root directory for getdents64. A symbolic
 * link at the end is followed, and one whose target is missing is what
 * O_CREAT creates, as on Linux, unless O_NOFOLLOW, which answers ELOOP, or
 * O_CREAT|O_EXCL, which answers EEXIST for the link. On a type that sets
 * `ns_serialised`, resolution through open-handle registration holds the
 * final mount's namespace lock, which is what keeps a writer from moving the
 * file between the lookup and the registration. O_PATH goes to `open_path`,
 * which names the file and opens nothing.
 *
 * A file the open creates is empty, so O_TRUNC does nothing to it, and
 * its description is marked OFD_WATCH_MADE, so that inotify reports no
 * truncation of it (`open_done`).
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
    struct vfs_mount *locked;
    char *through = NULL; /* the path a dangling link's target makes, in a path page */
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
    rc = vfs_resolve(0, name,
                     VFS_WALK_LOCK |
                         (((flags & O_NOFOLLOW) || ((flags & O_CREAT) && (flags & O_EXCL)))
                              ? VFS_WALK_NOFOLLOW
                              : 0),
                     &ref, 0, 0);
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
        rc = open_create(name, flags, mode & 07777u, &ref);
        if (rc > 0) {
            goto again;
        }
        if (rc != 0) {
            goto out;
        }
        flags &= ~O_TRUNC;
        current_proc->fds->entries[fd].ofd->watch_flags = OFD_WATCH_MADE;
    } else if (rc != 0) {
        goto out;
    } else {
        const struct vpath *vp = vfs_synth_vpath(ref.mnt, ref.node);

        if (vp) {
            struct vpath vpc = *vp;

            vfs_walk_unlock(ref.mnt);
            vfs_ref_release(&ref);
            fd_free(current_proc->fds, fd);
            fd = -1;
            rc = open_synth_node(&vpc, flags);
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
    rc = open_install(&ref, fd, name, flags);
    fd = -1;
    vfs_walk_unlock(locked);
out:
    if (fd >= 0) {
        fd_free(current_proc->fds, fd);
    }
    path_buf_put(through);
    return rc;
}

#define MS_REMOUNT 32

/**
 * mount(2) with MS_REMOUNT: the mount whose root `path` names takes the
 * flags `flags` (`vfs_remount`); -EINVAL for a path that is no mount's
 * root. A mount made read-only must have no descriptor open for writing on
 * it, -EBUSY, as on Linux.
 */
static int32_t remount_at(const char *path, uint32_t flags)
{
    struct vfs_ref ref;
    process_t *pr;
    int i, rc = vfs_resolve(0, path, 0, &ref, 0, 0);

    if (rc != 0) {
        return rc;
    }
    if (ref.node != ref.mnt->root) {
        vfs_ref_release(&ref);
        return -EINVAL;
    }
    if ((flags & VFS_MS_RDONLY) && !(ref.mnt->flags & VFS_MS_RDONLY)) {
        for (pr = proc_list; pr && rc == 0; pr = pr->next) {
            for (i = 0; pr->fds && i < fd_table_cap(pr->fds); i++) {
                fd_entry_t *e = &pr->fds->entries[i];

                if (e->type == FD_TYPE_FILE && (e->flags & FD_FLAG_WRITE) && e->ofd &&
                    e->ofd->vmnt == ref.mnt) {
                    rc = -EBUSY;
                    break;
                }
            }
        }
    }
    if (rc == 0) {
        rc = vfs_remount(ref.mnt, flags);
    }
    vfs_ref_release(&ref);
    return rc;
}

/**
 * mount(2): EBX = source, ECX = target, EDX = type, ESI = flags, EDI = data.
 *
 * Effective uid 0 is required. The strings are copied here; the order of
 * work and every other refusal are the VFS's (vfs_do_mount). `data` may be
 * NULL, which is an empty option string. With MS_REMOUNT the source, the
 * type and the data are not read, as on Linux for the flags here
 * (`remount_at`).
 */
int32_t sys_mount(isr_params_t *params)
{
    char *tpath;
    char source[64];
    char type[16];
    char data[128];
    int n;

    if (!current_proc) {
        return -ESRCH;
    }
    if (!cred_privileged()) {
        return -EPERM;
    }
    if (params->esi & MS_REMOUNT) {
        if (!params->ecx) {
            return -EFAULT;
        }
        n = path_from_user(AT_FDCWD, (const char __user *)params->ecx, &tpath);
        if (n == 0) {
            n = remount_at(tpath, params->esi & ~(uint32_t)MS_REMOUNT);
            path_buf_put(tpath);
        }
        return n;
    }
    if (!params->ebx || !params->ecx || !params->edx) {
        return -EFAULT;
    }
    n = strncpy_from_user(source, (const char __user *)params->ebx, sizeof(source));
    if (n < 0) {
        return n;
    }
    /* The target as an absolute canonical path: a relative one is joined
     * to the working directory, as every other path-taking call does. */
    n = path_from_user(AT_FDCWD, (const char __user *)params->ecx, &tpath);
    if (n != 0) {
        return n;
    }
    n = strncpy_from_user(type, (const char __user *)params->edx, sizeof(type));
    data[0] = '\0';
    if (n >= 0 && params->edi) {
        n = strncpy_from_user(data, (const char __user *)params->edi, sizeof(data));
    }
    if (n >= 0) {
        n = vfs_do_mount(source, tpath, type, params->esi, data);
    }
    path_buf_put(tpath);
    return n;
}

/**
 * umount2(2): EBX = target, ECX = flags. Effective uid 0 is required, and
 * the flags must be zero: MNT_FORCE, MNT_DETACH and UMOUNT_NOFOLLOW are
 * promises this kernel cannot keep and answer EINVAL.
 *
 * A target that names a directory by its end must reach one, which the walk
 * that finds the mount checks (`vfs_do_umount`), so a file named with a
 * trailing slash is -ENOTDIR, as on Linux, where a file named without one is
 * not a mount point, -EINVAL. The last "." put back comes off the target
 * first: a walk that meets a removed device answers -EIO, which
 * `vfs_do_umount` recovers from by the recorded target, and a recorded
 * target has none.
 */
int32_t sys_umount2(isr_params_t *params)
{
    char *tpath;
    int end;
    int n;

    if (!current_proc) {
        return -ESRCH;
    }
    if (!cred_privileged()) {
        return -EPERM;
    }
    if (params->ecx != 0) {
        return -EINVAL;
    }
    if (!params->ebx) {
        return -EFAULT;
    }
    n = path_from_user_dir(AT_FDCWD, (const char __user *)params->ebx, &tpath, &end);
    if (n != 0) {
        return n;
    }
    path_drop_dot(tpath, end);
    n = vfs_do_umount(tpath, end != 0);
    path_buf_put(tpath);
    return n;
}

/**
 * The removal rules for a directory on a type other than the root: write
 * permission on the directory, and in a sticky directory the entry's owner,
 * the directory's owner, or root. The root type keeps its own copy of the
 * rule beside its node fields.
 */
static int cred_remove_ref(const struct vfs_ref *parent, const char *leaf)
{
    struct vfs_stat pst, est;
    void *node;
    int rc = cred_access_ref(parent, CRED_W, 0);

    if (rc != 0 || cred_privileged()) {
        return rc;
    }
    rc = vfs_getattr(parent->mnt, parent->node, &pst);
    if (rc != 0 || !(pst.mode & 01000u) || pst.uid == current_proc->euid) {
        return rc;
    }
    rc = parent->mnt->type->ops->lookup(parent->mnt, parent->node, leaf, &node);
    if (rc != 0) {
        return rc == -ENOENT ? 0 : rc; /* a name not there yet is not held by anyone */
    }
    rc = vfs_getattr(parent->mnt, node, &est);
    if (parent->mnt->type->ops->put) {
        parent->mnt->type->ops->put(parent->mnt, node);
    }
    if (rc != 0) {
        return rc;
    }
    return est.uid == current_proc->euid ? 0 : -EPERM;
}

/**
 * Record in `ofd`, for inotify, the directory the open's `name` is in and
 * its last component, when they are the file opened: a name whose last
 * component was a link the open followed records nothing, where Linux
 * would report the target's directory.
 */
static void open_record_parent(struct open_file *ofd, const char *name)
{
    char leaf[VFS_NAME_MAX + 1];
    struct vfs_ref parent;
    struct vfs_stat dir, st;
    uint32_t len;
    void *found;

    if (vfs_resolve(0, name, VFS_WALK_PARENT, &parent, leaf, sizeof(leaf)) != 0) {
        return;
    }
    len = (uint32_t)k_strlen(leaf);
    if (parent.mnt == ofd->vmnt && len > 0 && !leaf_is_dot(leaf) &&
        vfs_getattr(parent.mnt, parent.node, &dir) == 0 && parent.mnt->type->ops->lookup &&
        parent.mnt->type->ops->lookup(parent.mnt, parent.node, leaf, &found) == 0) {
        if (vfs_getattr(parent.mnt, found, &st) == 0 && st.ino == ofd->ino) {
            ofd->leaf = kmalloc(len + 1);
            if (ofd->leaf) {
                k_memcpy(ofd->leaf, leaf, len + 1);
                ofd->parent_ino = dir.ino;
            }
        }
        if (parent.mnt->type->ops->put) {
            parent.mnt->type->ops->put(parent.mnt, found);
        }
    }
    vfs_ref_release(&parent);
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
    if (truncated) {
        filemap_file_truncated(ofd->vmnt, ofd->ino, 0);
    }
    if (!inotify_watches) {
        return;
    }
    open_record_parent(ofd, name);
    if (truncated) {
        inotify_desc(ofd, IN_MODIFY);
    }
    inotify_desc(ofd, IN_OPEN);
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
 */
static int32_t open_from_user(int dirfd, const char __user *upath, int flags, uint32_t mode)
{
    char *path;
    int end;
    int32_t rc;

    if ((flags & (O_CREAT | O_DIRECTORY | O_PATH)) == (O_CREAT | O_DIRECTORY)) {
        return -EINVAL;
    }
    if (!upath) {
        return -EFAULT;
    }
    rc = path_from_user_dir(dirfd, upath, &path, &end);
    if (rc != 0) {
        return rc;
    }
    rc = open_named(path_name(path), flags, mode, end);
    if (rc >= 0) {
        open_done(rc, path_name(path), flags);
    }
    path_buf_put(path);
    return rc;
}

/* open(2): EBX = path, ECX = flags, EDX = the O_CREAT mode. */
int32_t sys_open(isr_params_t *params)
{
    if (!current_proc) {
        return -EFAULT;
    }
    return open_from_user(AT_FDCWD, (const char __user *)params->ebx, (int)params->ecx,
                          params->edx);
}

/*
 * openat (syscall 295): EBX = dirfd, ECX = path, EDX = flags, ESI = the
 * O_CREAT mode. musl's openat() issues it; its open() and <stdio.h> use the
 * legacy open on i386, which has the number. An absolute path is taken
 * as-is, a relative path resolves against the cwd (AT_FDCWD) or against the
 * directory the dirfd refers to (see copy_path_at).
 */
int32_t sys_openat(isr_params_t *params)
{
    if (!current_proc) {
        return -EFAULT;
    }
    return open_from_user((int)params->ebx, (const char __user *)params->ecx, (int)params->edx,
                          params->esi);
}

/** Whether a call of a thread other than the caller that shares `table`
 * holds descriptor `fd`: has looked it up since the call began. */
static int fd_held_elsewhere(const fd_table_t *table, int fd)
{
    const process_t *t;

    if (table->refs < 2) {
        return 0;
    }
    for (t = proc_list; t; t = t->next) {
        if (t != current_proc && t->fds == table && (t->fd_held[fd / 32] & (1u << (fd % 32)))) {
            return 1;
        }
    }
    return 0;
}

/**
 * Release descriptor `fd` of `table`, with every backing it holds but the
 * record locks, which the close that ends it drops itself, and free its
 * slot. The entry is marked closing and releasing first, since the release
 * may sleep, in a file system's release of the description's last reference
 * among other places: while it does, a lookup, a dup onto the slot, a copy
 * of the table and another release all pass the entry by, and none of them
 * can take a reference to a description that is going.
 */
static void fd_teardown(fd_table_t *table, int fd)
{
    fd_entry_t *e = &table->entries[fd];

    e->flags |= FD_FLAG_CLOSING | FD_FLAG_RELEASING;
    fd_release_objects(e);
    fd_free(table, fd);
}

/**
 * Close descriptor `fd` of the caller's table. The table's record locks on
 * the file go at once, as Linux drops them at every close. Then the entry
 * is released and its slot freed (`fd_teardown`), or, while a call of
 * another thread sharing the table holds it, marked closing, so that the
 * call ends with what it had and the slot is released as that call ends
 * (`fd_holds_release`), which drops no locks: that may be the call of
 * another process sharing the table, or a call of the caller's own that
 * ends after it took a lock again. Linux keeps the file for such a call
 * through the reference `fdget` takes. 0, or -EBADF.
 */
static int32_t close_fd(int fd)
{
    fd_table_t *table = current_proc->fds;
    fd_entry_t *e = fd_get(table, fd);

    if (!e) {
        return -EBADF;
    }
    fd_close_locks(e);
    if (fd_held_elsewhere(table, fd)) {
        e->flags |= FD_FLAG_CLOSING;
        return 0;
    }
    fd_teardown(table, fd);
    return 0;
}

/**
 * The end of a call's holds (syscall_internal.h). An entry whose release
 * another thread has begun is left to it: two threads whose calls end while
 * one of them sleeps in a release would otherwise both find the entry
 * closing and held by nobody, and release it twice.
 */
void fd_holds_release(void)
{
    process_t *p = current_proc;
    int w;

    if (!p || !p->fd_holding) {
        return;
    }
    p->fd_holding = 0;
    for (w = 0; w < FD_TABLE_LIMIT / 32; w++) {
        uint32_t bits = p->fd_held[w];
        int fd;

        p->fd_held[w] = 0;
        for (fd = w * 32; bits != 0; fd++, bits >>= 1) {
            fd_entry_t *e;

            if (!(bits & 1u) || !p->fds || fd >= p->fds->cap) {
                continue;
            }
            e = &p->fds->entries[fd];
            if ((e->flags & (FD_FLAG_CLOSING | FD_FLAG_RELEASING)) == FD_FLAG_CLOSING &&
                !fd_held_elsewhere(p->fds, fd)) {
                fd_teardown(p->fds, fd);
            }
        }
    }
}

#define CLOSE_RANGE_UNSHARE 0x2u
#define CLOSE_RANGE_CLOEXEC 0x4u

/**
 * close_range(2), system call 436: EBX = first, ECX = last, EDX = flags.
 * Closes each open descriptor from `first` to `last`, or with
 * CLOSE_RANGE_CLOEXEC marks each close-on-exec instead; CLOSE_RANGE_UNSHARE
 * first gives a caller that shares its table a copy of its own. A number
 * past the table is no descriptor. 0, or -EINVAL for a flag it does not
 * take or `first` past `last`, or -ENOMEM.
 */
int32_t sys_close_range(isr_params_t *params)
{
    uint32_t first = params->ebx;
    uint32_t last = params->ecx;
    uint32_t flags = params->edx;
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

/* close(2): EBX = fd. Returns 0 or -EBADF. Closing 0, 1 or 2 is legal; a
 * shell or daemon closes one and opens again onto the freed (lowest)
 * number to redirect it. */
int32_t sys_close(isr_params_t *params)
{
    if (!current_proc) {
        return -ESRCH;
    }
    return close_fd((int)params->ebx);
}

/**
 * Flush a file's data to stable storage (fsync(2); also serves fdatasync).
 *
 * EBX = fd. Validates the descriptor (any open fd but an O_PATH one; the type
 * is not checked).
 * A descriptor on a mounted volume goes to the type's fsync through
 * `vfs_fsync`; any other writes back every dirty page of the disk-backed
 * window via `pc_sync`. That flush is whole-cache, not per-file, a safe
 * superset of the contract. When KRFFS is the RAM module the cache is empty,
 * so the flush is a no-op and the call only validates the fd. Returns 0,
 * -EBADF for a bad
 * descriptor, or -EIO when a disk writeback fails. dropbear writes its host
 * key with the open/write/fsync/rename idiom and treats an fsync error as
 * fatal, so the -EIO is real backpressure.
 */
int32_t sys_fsync(isr_params_t *params)
{
    int fd = (int)params->ebx;

    if (!current_proc) {
        return -ESRCH;
    }
    {
        fd_entry_t *e = fd_get(current_proc->fds, fd);

        if (!e || (e->flags & FD_FLAG_PATH)) {
            return -EBADF;
        }
        if (e->ofd && e->ofd->vmnt) {
            int32_t mapped = filemap_sync_file(e->ofd->vmnt, e->ofd->ino);
            int32_t rc = vfs_fsync(e->ofd->vmnt, e->ofd->fhandle);

            return mapped != 0 ? -EIO : rc;
        }
    }
    if (pc_sync() != 0) {
        return -EIO;
    }
    return 0;
}

/**
 * Flush all buffered file-system writes to stable storage (sync(2)).
 *
 * Takes no argument. Every active mount's `sync` runs and the buffer cache
 * writes its dirty buffers, then every dirty page of the disk-backed window
 * goes back via `pc_sync` (a no-op when KRFFS is the RAM module). Like
 * Linux's sync(2), it returns 0 even if a writeback failed; what failed
 * stays dirty for a later call, and a mount whose device failed a write is
 * read-only from then on. Busy slots are retried after yielding and an
 * interrupt-enabled wait, so an in-flight write can finish and a page
 * dirtied during that write can be flushed. Stop after 3000 ticks (30
 * seconds) if slots remain busy, so a stuck writer cannot hold this call
 * forever. A write failure alone does not cause a wait.
 */
int32_t sys_sync(isr_params_t *params)
{
    uint32_t start;

    (void)params;
    filemap_sync_all();
    vfs_sync_all();
    start = kernel_ticks;
    while (pc_sync() & PC_SYNC_BUSY) {
        if ((uint32_t)(kernel_ticks - start) >= 3000u) {
            break;
        }
        schedule();
        idle_wait_irq_on();
    }
    return 0;
}

/**
 * Flush the file system a descriptor is on (syncfs(2), syscall 344).
 *
 * EBX = fd. The kernel keeps no list of a mount's own dirty buffers, so
 * this writes back everything, as sync(2) does, and like it returns 0;
 * GNU `sync -f` and BusyBox's `sync -f` call it. -EBADF for a descriptor
 * that is not open or was opened with O_PATH, as on Linux.
 */
int32_t sys_syncfs(isr_params_t *params)
{
    fd_entry_t *e;

    if (!current_proc) {
        return -ESRCH;
    }
    e = fd_get(current_proc->fds, (int)params->ebx);
    if (!e || (e->flags & FD_FLAG_PATH)) {
        return -EBADF;
    }
    return sys_sync(params);
}

/**
 * Advise the kernel of reads to come (readahead(2), syscall 225).
 *
 * EBX = fd, ECX:EDX = the offset, ESI = the count. The page cache fills on
 * demand, so there is nothing to start early, and the call returns 0, as
 * Linux does once it has queued the reads. -EBADF for a descriptor not open
 * for reading, and -EINVAL for one that is not a regular file, as on Linux.
 */
int32_t sys_readahead(isr_params_t *params)
{
    fd_entry_t *e;

    if (!current_proc) {
        return -ESRCH;
    }
    e = fd_get(current_proc->fds, (int)params->ebx);
    if (!e || (e->flags & FD_FLAG_PATH) || !(e->flags & FD_FLAG_READ)) {
        return -EBADF;
    }
    return e->type == FD_TYPE_FILE ? 0 : -EINVAL;
}

/**
 * Check one advice of posix_fadvise(3) and ignore it.
 *
 * The page cache has one policy for every file, so the advice changes
 * nothing; GNU `cp`, `cat` and `sort` give it and do not look at the
 * result. The call fails as Linux's does: -EBADF for a descriptor not open,
 * -ESPIPE for a pipe or a FIFO, and -EINVAL for a negative length or an
 * advice outside POSIX_FADV_NORMAL (0) to POSIX_FADV_NOREUSE (5).
 */
static int32_t fadvise(int fd, uint32_t len_hi, uint32_t advice)
{
    fd_entry_t *e;

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
    return (len_hi & 0x80000000u) || advice > 5 ? -EINVAL : 0;
}

/**
 * fadvise64(250): EBX = fd, ECX:EDX = the offset, ESI = a 32-bit length,
 * EDI = the advice.
 */
int32_t sys_fadvise64(isr_params_t *params)
{
    return fadvise((int)params->ebx, 0, params->edi);
}

/**
 * fadvise64_64(272), which musl calls: EBX = fd, ECX:EDX = the offset,
 * ESI:EDI = the length, EBP = the advice.
 */
int32_t sys_fadvise64_64(isr_params_t *params)
{
    return fadvise((int)params->ebx, params->edi, params->ebp);
}

/* The i386 requests of chattr(1) and lsattr(1): _IOR and _IOW of 'f', and
   _IOR of 'v'. */
#define FS_IOC_GETFLAGS   0x80046601u
#define FS_IOC_SETFLAGS   0x40046602u
#define FS_IOC_GETVERSION 0x80047601u

/**
 * FS_IOC_GETFLAGS, FS_IOC_SETFLAGS and FS_IOC_GETVERSION on a descriptor,
 * which set `*handled`.
 *
 * Of the types here only ext2 keeps the flags and the generation, and only
 * for a file or a directory; any other descriptor leaves
 * the request to the rest of ioctl(2), which answers -ENOTTY, as Linux does
 * for a file system without them. The flags are read from the inode and
 * acted on by nothing, so a change is -EOPNOTSUPP: an immutable or
 * append-only flag stored and not enforced would protect nothing. Setting
 * the flags a file already has succeeds for its owner, which is what
 * `chattr -i` does to an ordinary file; another caller is -EPERM, as there.
 */
int32_t fs_flags_ioctl(fd_entry_t *entry, uint32_t request, uint32_t arg, int *handled)
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
    if (!cred_privileged() && current_proc->euid != st.uid) {
        return -EPERM;
    }
    return want == st.flags ? 0 : -EOPNOTSUPP;
}

#define XATTR_FOLLOW 0 /* a path, a final symbolic link followed */
#define XATTR_LINK   1 /* a path, the link itself */
#define XATTR_FD     2 /* a descriptor */

/**
 * An extended attribute call on the file EBX names, as a path or as a
 * descriptor by `how`, which returns `result` once the file is found.
 *
 * No file system here stores extended attributes, and Linux answers for a
 * file system built without them: get, set and remove fail with
 * -EOPNOTSUPP and a list is empty. The name is resolved first, so a missing
 * file is -ENOENT and a bad descriptor -EBADF, as there; `cp -a`, `tar` and
 * `rsync -X` read -EOPNOTSUPP as a file system without attributes and go on.
 */
static int32_t xattr_call(isr_params_t *params, int how, int32_t result)
{
    const char __user *upath = (const char __user *)params->ebx;
    struct vfs_ref ref;
    fd_entry_t *e;
    char *path;
    int dir;
    int32_t rc;

    if (!current_proc) {
        return -ESRCH;
    }
    if (how == XATTR_FD) {
        e = fd_get(current_proc->fds, (int)params->ebx);
        return !e || (e->flags & FD_FLAG_PATH) ? -EBADF : result;
    }
    if (!upath) {
        return -EFAULT;
    }
    rc = path_from_user_dir(AT_FDCWD, upath, &path, &dir);
    if (rc != 0) {
        return rc;
    }
    rc = resolve_named(path_name(path), how == XATTR_LINK ? VFS_WALK_NOFOLLOW : 0, dir, &ref);
    path_buf_put(path);
    if (rc != 0) {
        return rc;
    }
    vfs_ref_release(&ref);
    return result;
}

/* setxattr(226) to fremovexattr(237), in Linux's order. */
int32_t sys_setxattr(isr_params_t *params)
{
    return xattr_call(params, XATTR_FOLLOW, -EOPNOTSUPP);
}

int32_t sys_lsetxattr(isr_params_t *params)
{
    return xattr_call(params, XATTR_LINK, -EOPNOTSUPP);
}

int32_t sys_fsetxattr(isr_params_t *params)
{
    return xattr_call(params, XATTR_FD, -EOPNOTSUPP);
}

int32_t sys_getxattr(isr_params_t *params)
{
    return xattr_call(params, XATTR_FOLLOW, -EOPNOTSUPP);
}

int32_t sys_lgetxattr(isr_params_t *params)
{
    return xattr_call(params, XATTR_LINK, -EOPNOTSUPP);
}

int32_t sys_fgetxattr(isr_params_t *params)
{
    return xattr_call(params, XATTR_FD, -EOPNOTSUPP);
}

int32_t sys_listxattr(isr_params_t *params)
{
    return xattr_call(params, XATTR_FOLLOW, 0);
}

int32_t sys_llistxattr(isr_params_t *params)
{
    return xattr_call(params, XATTR_LINK, 0);
}

int32_t sys_flistxattr(isr_params_t *params)
{
    return xattr_call(params, XATTR_FD, 0);
}

int32_t sys_removexattr(isr_params_t *params)
{
    return xattr_call(params, XATTR_FOLLOW, -EOPNOTSUPP);
}

int32_t sys_lremovexattr(isr_params_t *params)
{
    return xattr_call(params, XATTR_LINK, -EOPNOTSUPP);
}

int32_t sys_fremovexattr(isr_params_t *params)
{
    return xattr_call(params, XATTR_FD, -EOPNOTSUPP);
}

/**
 * Add a seek displacement to a file offset without overflowing.
 *
 * The sum of an offset and a displacement leaves the range of the type
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

/** Seek a raw description after any transfer using its offset completes. */
static int32_t raw_lseek(fd_entry_t *entry, int32_t offset, int whence)
{
    uint64_t resolved, raw_size;
    int32_t rc;

    fd_ofd_lock(entry->ofd);
    if (!devfs_fd_raw_size(entry, &raw_size)) {
        fd_ofd_unlock(entry->ofd);
        return -ENXIO;
    }
    rc = raw_seek_resolve(entry->ofd->raw_offset, raw_size, (long long)offset, whence, &resolved);
    if (rc == 0 && resolved > (uint64_t)0x7FFFFFFF) {
        rc = -EOVERFLOW;
    }
    if (rc == 0) {
        entry->ofd->raw_offset = resolved;
    }
    fd_ofd_unlock(entry->ofd);
    return rc != 0 ? rc : (int32_t)resolved;
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
 * lseek(2) and _llseek(2) on a directory or a /proc file: the offset
 * `whence` and `off` give, stored in the description under its lock and
 * returned in `*out`, or a negative errno with the offset unchanged. A read
 * or a write of a /proc file holds the same lock (`ops_offset_held`), so a
 * seek waits for one that sleeps.
 *
 * SEEK_SET and SEEK_CUR move the offset; SEEK_END is -EINVAL, as Linux's
 * /proc files and tmpfs directories answer, since neither has a size to
 * seek from. A /proc file's offset is a byte of its text, the snapshot its
 * open took or the text a read of oom_score_adj makes, or, for
 * /proc/PID/mem, an address, so it takes any place through 0xFFFFFFFF, the
 * last address. A directory's offset is the cursor of its listing, which
 * getdents64 reports after each entry as the entry's d_off: a directory of
 * /dev or /proc takes the index of any entry through 0x7FFFFFFF, and one of
 * a file system the places `dir_cursor_valid` finds, so rewinddir, and
 * seekdir to a place telldir gave, move it, and no type's `readdir` is given
 * a place it did not give. Any other place is -EINVAL, and one past `limit`,
 * the largest offset the call can return, -EOVERFLOW.
 */
static int seek_listing(fd_entry_t *entry, long long off, int whence, uint64_t limit,
                        long long *out)
{
    uint64_t most = entry->type == FD_TYPE_SYNTH_DIR ? 0x7FFFFFFFull : 0xFFFFFFFFull;
    long long at = off;
    int rc = 0;

    if (!entry->ofd || (entry->type == FD_TYPE_DIR && !entry->ofd->vmnt)) {
        return -EBADF;
    }
    if (entry->type == FD_TYPE_DIR && fd_node_check(entry) != 0) {
        return -EBADF; /* the directory was removed out from under this fd */
    }
    fd_ofd_lock(entry->ofd);
    if (whence == 1) {
        rc = seek_add(entry->ofd->offset, off, &at);
    } else if (whence != 0) {
        rc = -EINVAL;
    }
    if (rc == 0 && (at < 0 || (uint64_t)at > most)) {
        rc = -EINVAL;
    }
    if (rc == 0 && entry->type == FD_TYPE_DIR) {
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

int32_t sys_lseek(isr_params_t *params)
{
    int fd = (int)params->ebx;
    int32_t offset = (int32_t)params->ecx;
    int whence = (int)params->edx;
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
    if (entry->type == FD_TYPE_PIPE) {
        return -ESPIPE; /* a pipe is not seekable (POSIX) */
    }
    if (entry->type == FD_TYPE_DEVICE) {
        if (devfs_fd_is_raw(entry)) {
            return raw_lseek(entry, offset, whence);
        }
        if (devfs_fd_log_size(entry, NULL)) {
            seek_rc = seek_bootlog(entry, (long long)offset, whence, 0x7FFFFFFFull, 1, &at);
            return seek_rc != 0 ? seek_rc : (int32_t)at;
        }
        /* mem devices (null/zero/full) accept a seek that always lands at 0;
         * the stream devices (random/urandom) are not seekable. */
        if (devfs_lseek_zero(entry)) {
            entry->ofd->offset = 0;
            return 0;
        }
        return -ESPIPE;
    }
    if (entry->type == FD_TYPE_CONSOLE || entry->type == FD_TYPE_MOUSE) {
        return -ESPIPE; /* a terminal/mouse is a valid fd, just not seekable */
    }
    if (seeks_listing(entry)) {
        seek_rc = seek_listing(entry, (long long)offset, whence, 0x7FFFFFFFull, &at);
        return seek_rc != 0 ? seek_rc : (int32_t)at;
    }
    if (entry->type != FD_TYPE_FILE) {
        return -EBADF;
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
        at = (long long)offset;
        seek_rc = 0;
        break;
    case 1: /* SEEK_CUR */
        seek_rc = seek_add(entry->ofd->offset, (long long)offset, &at);
        break;
    case 2: /* SEEK_END */
        seek_rc = seek_add(file_size, (long long)offset, &at);
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
    if (at > (long long)0x7FFFFFFF) {
        fd_ofd_unlock(entry->ofd);
        return -EOVERFLOW; /* a real offset this call cannot return; llseek can */
    }

    entry->ofd->offset = (uint64_t)at;
    fd_ofd_unlock(entry->ofd);
    return (int32_t)at;
}

/**
 * Whether this is the query descriptor rather than a socket that carries data.
 *
 * A DHCP client opens AF_INET/SOCK_RAW/IPPROTO_RAW to ask the interface
 * ioctls about the machine and does nothing else with it. It has no protocol,
 * so it can send and receive nothing; a raw socket with a protocol is a real
 * one, and the paths below tell them apart here rather than by socket type.
 */
int sock_is_query(const struct socket *sock)
{
    return sock->type == SOCK_RAW && sock->protocol == 0;
}

/** Dispatch an ops-backed read, holding the description of one whose read
 * uses its offset (`ops_offset_held`). */
static int32_t read_ops(fd_entry_t *entry, char __user *buf, uint32_t count)
{
    struct open_file *held = ops_offset_held(entry);
    int32_t rc;

    if (!(entry->flags & FD_FLAG_READ)) {
        return -EBADF;
    }
    if (held) {
        fd_ofd_lock(held);
    }
    rc = entry->ops->read ? entry->ops->read(entry, buf, count) : -EBADF;
    if (held) {
        fd_ofd_unlock(held);
    }
    return rc;
}

/**
 * Read up to `count` bytes into user buffer `buf` from the file of a mounted
 * type or of the KRFFS root that `entry` is open on: at the description's
 * offset, which moves past what was read, or at `*at` for pread64, which
 * leaves it where it was.
 *
 * The file system writes straight into the user buffer, so its pages are
 * range-checked and faulted in first. The description is held across the
 * operation: with dup or fork sharing the offset, a second reader sleeps
 * rather than reading the same bytes while the first is in a request, and a
 * KRFFS relocation cannot move the handle while it is in use.
 */
static int32_t file_read(fd_entry_t *entry, char __user *buf, uint32_t count, const uint64_t *at)
{
    struct open_file *ofd = entry->ofd;
    int32_t bytes_read;

    if (!(entry->flags & FD_FLAG_READ)) {
        return -EBADF;
    }
    if (!ofd || !ofd->vmnt) {
        return -EBADF;
    }
    if (!user_buffer_ok((uint32_t)(__force uintptr_t)buf, count, 1)) {
        return -EFAULT;
    }
    if (filemap_file_reading(ofd->vmnt, ofd->ino, at ? *at : ofd->offset, count) != 0) {
        return -EIO;
    }
    fd_ofd_lock(ofd);
    bytes_read =
        vfs_read(ofd->vmnt, ofd->fhandle, (char __force *)buf, count, at ? *at : ofd->offset);
    if (bytes_read > 0 && !at) {
        ofd->offset += (uint64_t)bytes_read;
    }
    fd_ofd_unlock(ofd);
    if (bytes_read > 0 && inotify_watches) {
        inotify_desc(ofd, IN_ACCESS);
    }
    return bytes_read;
}

/**
 * Read up to `count` bytes from descriptor `fd` (read(2)).
 *
 * EBX = fd, ECX = buf, EDX = count. Returns the bytes read, 0 at end, or
 * -errno. Dispatches by backing kind. A console read (stdin) blocks in the
 * tty line discipline until a cooked line or VMIN raw bytes arrive; in
 * graphics mode it is a non-blocking raw scancode read instead.
 */
static int32_t read_fd(int fd, char __user *buf, uint32_t count)
{
    fd_entry_t *entry;

    if (!current_proc) {
        return -EFAULT;
    }

    entry = fd_get(current_proc->fds, fd);
    if (!entry || (entry->flags & FD_FLAG_PATH)) {
        return -EBADF;
    }

    /* A zero-length read is a no-op that returns 0 (even with a NULL buffer,
     * per POSIX) once the fd is known valid; do not touch buf. */
    if (count == 0) {
        return 0;
    }
    if (!buf) {
        return -EFAULT;
    }

    /* Backing-ops object (pipe/tty/pty/device): dispatch through its vtable,
     * but only if the descriptor was opened for reading (a write-only fd ->
     * -EBADF). The plain file and enum types below have no ops and fall through
     * to the enum dispatch. */
    if (entry->ops) {
        return read_ops(entry, buf, count);
    }

    /* read() honors O_NONBLOCK (an OFD status flag) exactly like recv(),
     * routing to the non-blocking variant. */
    if (entry->type == FD_TYPE_SOCKET) {
        int sock_nonblock;
        if (!entry->sock || !user_buffer_ok((uint32_t)buf, count, 1)) {
            return -EFAULT;
        }
        if (sock_is_query(entry->sock)) {
            return -EOPNOTSUPP;
        }
        sock_nonblock = entry->ofd && (entry->ofd->status_flags & OFD_NONBLOCK);
        /* socket_*recv write straight into the validated user buffer; __force
         * strips the __user tag at the boundary. */
        if (entry->sock->type == SOCK_DGRAM || entry->sock->type == SOCK_RAW) {
            if (sock_nonblock) {
                return socket_try_recv(entry->sock, (uint8_t __force *)buf, (int)count, NULL, NULL,
                                       0);
            }
            return socket_recv(entry->sock, (uint8_t __force *)buf, (int)count, NULL, NULL, 0);
        }
        if (sock_nonblock) {
            return socket_tcp_try_recv(entry->sock, (uint8_t __force *)buf, (int)count, 0);
        }
        return socket_tcp_recv(entry->sock, (uint8_t __force *)buf, (int)count, 0);
    }

    if (entry->type == FD_TYPE_CONSOLE) {
        unsigned char kbuf[N_TTY_READ_CHUNK];
        uint32_t want;
        int n;

        if (!(entry->flags & FD_FLAG_READ)) {
            return -EBADF; /* Not readable (e.g., stdout/stderr) */
        }

        /*
         * If the process owns the framebuffer (graphics mode), use non-blocking
         * raw scancode input for game controls, bypassing the line discipline.
         */
        if (fb_owner_pid == current_proc->tgid) {
            /* Non-blocking raw scancode read for games. kbd_read_scancodes
             * writes into the user buffer; range-check it first, then __force
             * strips the __user tag at the boundary. */
            if (!user_buffer_ok((uint32_t)buf, count, 1)) {
                return -EFAULT;
            }
            return kbd_read_scancodes((uint8_t __force *)buf, (int)count);
        }

        want = count;
        if (want > sizeof(kbuf)) {
            want = sizeof(kbuf); /* one line/burst per call; caller loops */
        }

        /*
         * Park on console_tty until a line (cooked) or VMIN bytes (raw)
         * arrive. Parking marks input_wq active, and once a session owns the
         * console its sid is nonzero; either condition tells the keyboard
         * IRQ to cook keystrokes into console_tty, not the kernel
         * shell, so the byte wakes us (or waits as type-ahead).
         * stdin_waiting_proc, set across the read below, only suppresses the
         * scrollback hotkeys while we are parked; tty_read drains the serial
         * port itself. The discipline echoes, edits, and recognizes ISIG, so
         * Ctrl-C on the console now arrives as a posted SIGINT, and a
         * pending signal unwinds the wait with -EINTR.
         */
        /* A SESSION LEADER reading the console with no controlling terminal
         * claims it as its SOLE ctty (its group becomes the foreground, and any
         * pty it held is dropped), so a lone program receives ^C; a non-leader
         * does not (it would strand the tty on its since-dead leader). Once a
         * session owns the console, the foreground is set by TIOCSPGRP. */
        if (console_tty.sid == 0 && (int)current_proc->sid == (int)current_proc->pid) {
            tty_make_sole_ctty(&console_tty, (int)current_proc->sid, (int)current_proc->pgid);
        }
        /* A BACKGROUND process reading its controlling terminal is stopped with
         * SIGTTIN (POSIX), unless that would wedge it (SIGTTIN blocked or
         * ignored), in which case the read fails with -EIO. Decided BEFORE
         * marking ourselves the console reader, so an early return leaves no
         * stale flag. */
        if (tty_is_background(&console_tty, (int)current_proc->sid, (int)current_proc->pgid)) {
            if ((current_proc->sig_blocked & KSIG_BIT(SIGTTIN)) ||
                current_proc->sigacts[SIGTTIN].handler == KSIG_IGN) {
                return -EIO;
            }
            tty_signal_group((int)current_proc->pgid, SIGTTIN);
            return -EINTR; /* the SIGTTIN stops us; the read is retried when fg */
        }
        stdin_waiting_proc = current_proc;
        n = tty_read(&console_tty, kbuf, (int)want,
                     entry->ofd && (entry->ofd->status_flags & OFD_NONBLOCK));
        stdin_waiting_proc = NULL;
        if (n < 0) {
            return n; /* -EINTR: a signal cut the wait short */
        }
        if (copy_to_user(buf, kbuf, (uint32_t)n) != 0) {
            return -EFAULT;
        }
        return n;
    }

    if (entry->type == FD_TYPE_MOUSE) {
        int packets_requested;
        int packets_read;

        packets_requested = (int)(count / sizeof(struct mouse_packet));
        if (packets_requested <= 0) {
            return 0; /* Buffer too small for even one packet */
        }

        /* Read packets from the mouse ring buffer. mouse_read_packets writes
         * into the user buffer; range-check it, then __force at the boundary.
         */
        if (!user_buffer_ok((uint32_t)buf,
                            (uint32_t)packets_requested * sizeof(struct mouse_packet), 1)) {
            return -EFAULT;
        }
        packets_read = mouse_read_packets((struct mouse_packet __force *)buf, packets_requested);

        return packets_read * (int32_t)sizeof(struct mouse_packet);
    }

    if (entry->type == FD_TYPE_FILE) {
        return file_read(entry, buf, count, NULL);
    }

    return -EBADF;
}

/**
 * read(2): EBX = fd, ECX = buf, EDX = count.
 *
 * An eventfd and a timerfd read whole eight-byte counts and refuse a
 * shorter read with EINVAL, as Linux does, and a read of zero bytes among
 * them: such a read goes to their read method, before the zero-length
 * shortcut of `read_fd`, which readv(2) of nothing keeps, as Linux's does.
 */
int32_t sys_read(isr_params_t *params)
{
    fd_entry_t *e = current_proc ? fd_get(current_proc->fds, (int)params->ebx) : NULL;

    if (params->edx == 0 && e && e->type == FD_TYPE_ANON && e->ops && e->ops->read) {
        return read_ops(e, (char __user *)params->ecx, 0);
    }
    return read_fd((int)params->ebx, (char __user *)params->ecx, params->edx);
}

/**
 * Read into `buf` only if that can be done without waiting. Returns the bytes
 * read, 0 when there are none, or a negative errno.
 *
 * This is what fills the second and later buffers of a `readv` on anything
 * whose bytes arrive, not sit there. Readiness is asked of the backing
 * object itself: the ops vtable answers `poll`, and a stream socket has a
 * non-blocking read of its own. No other process can consume the bytes
 * between the question and the read, since a system call is not preempted
 * here, and an interrupt only adds bytes.
 *
 * A terminal answers 0 whatever it has. Its read returns one line, and a
 * second line into the next buffer would answer a question the reader did not
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

        return read_ops(entry, buf, count);
    }
    if (entry->type == FD_TYPE_SOCKET && entry->sock) {
        int32_t r;

        if (sock_is_query(entry->sock)) {
            return -EOPNOTSUPP;
        }
        if (!user_buffer_ok((uint32_t)buf, count, 1)) {
            return -EFAULT;
        }
        if (entry->sock->type == SOCK_RAW) {
            return socket_try_recv(entry->sock, (uint8_t __force *)buf, (int)count, NULL, NULL, 0);
        }
        r = socket_tcp_try_recv(entry->sock, (uint8_t __force *)buf, (int)count, 0);

        return (r == SOCK_EAGAIN) ? 0 : r;
    }

    return read_fd(fd, buf, count);
}

/**
 * Fill the buffers of a `readv` from one waiting datagram.
 *
 * A datagram is a message, so it is taken once and copied out in pieces: the
 * buffers are filled in order until the datagram runs out, and what is left
 * over when the buffers run out first is dropped, which is what a `read` too
 * small for a datagram already does. Exactly one datagram is consumed either
 * way, and a datagram of no bytes returns 0 without being end of input.
 *
 * The message is taken out of the socket whole and under one hold, and only
 * then spread across the buffers. Both halves of that are deliberate.
 *
 * Taking it under one hold is not optional: a syscall body runs with interrupts
 * ENABLED (only preemption is off, see net.h), so the receive interrupt would
 * otherwise deliver the next datagram over this one between two buffers, and
 * the caller would get a prefix of one message followed by bytes of another.
 *
 * Spreading it afterwards, with interrupts on and through `copy_to_user`, is
 * what keeps a caller's own iovec array from being a weapon. The buffers are
 * read from user memory as the copy goes, so a caller can point one buffer AT
 * its own vector list and have the datagram rewrite the destinations behind it.
 * Those addresses were never checked by the pass that ran before the read.
 * `copy_to_user` refuses an address it cannot fault in, which makes such a
 * caller a process scribbling on itself, not a raw kernel write to
 * whatever it named.
 *
 * The message buffer is one datagram wide on a 16 KB kernel stack. That is the
 * whole of what "a kernel buffer to read into" ever needed to be.
 *
 * `src_ip` and `src_port` report the datagram's sender for a caller that has
 * somewhere to put it, and are NULL for one that has not; the sender is read
 * under the same hold as the copy, since consuming is what publishes it. With
 * `peek` the datagram is copied and left waiting for the next receive, as
 * recvmsg's MSG_PEEK asks. `whole`, when not NULL, is given the datagram's
 * length, which is more than the bytes copied when the buffers took less,
 * for recvmsg's MSG_TRUNC.
 *
 * Returns the bytes copied, or a negative errno.
 */
int32_t readv_dgram(fd_entry_t *entry, uint32_t iov_user, int iovcnt, uint32_t *src_ip,
                    uint16_t *src_port, int peek, int32_t *whole)
{
    uint8_t message[SOCK_RECV_BUF_SIZE];
    struct kernel_iovec iov;
    uint32_t offset = 0;
    int32_t total = 0;
    uint32_t held;
    int nonblock;
    int taken;
    int i;

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
    }

    held = net_lock();
    taken = socket_dgram_peek(entry->sock, 0, message, (int)sizeof(message));
    if (taken < 0) {
        taken = 0; /* it went between the wait and the hold */
    }
    if (peek) {
        socket_dgram_sender(entry->sock, src_ip, src_port);
    } else {
        socket_dgram_consume(entry->sock, src_ip, src_port);
    }
    net_unlock(held);
    if (whole) {
        *whole = taken;
    }

    for (i = 0; i < iovcnt && (int)offset < taken; i++) {
        uint32_t chunk;

        if (copy_from_user(
                &iov, (const void __user *)(iov_user + (uint32_t)i * sizeof(struct kernel_iovec)),
                sizeof(iov)) != 0) {
            break;
        }
        if (iov.iov_len == 0) {
            continue;
        }
        chunk = (uint32_t)taken - offset;
        if (chunk > iov.iov_len) {
            chunk = iov.iov_len;
        }
        if (copy_to_user((void __user *)iov.iov_base, message + offset, chunk) != 0) {
            break; /* the caller moved its own buffer out from under itself */
        }
        offset += chunk;
        total += (int32_t)chunk;
        if (chunk < iov.iov_len) {
            break;
        }
    }

    return total;
}

/*
 * Scatter read (readv(2)). The twin of `writev` above, and it exists for the
 * same reason that one does: musl's buffered stdio issues this call rather than
 * `read` whenever it is filling more than a single byte, so without it every
 * program that reads through `fgets`, `fread` or `scanf` fails on this system.
 * That is why several programs in this tree read a byte at a time through
 * `read` instead, and why busybox's `chpasswd`, which reads its input with
 * `xmalloc_fgetline`, exited without a message before this existed.
 *
 * EBX = fd, ECX = struct iovec[], EDX = iovcnt. Returns the total bytes read
 * across the iovecs, or a negative errno.
 *
 * The whole vector list is checked before a byte is taken, because a read
 * cannot be undone: a caller that named a bad destination in its last buffer
 * gets -EFAULT with its input still queued, not a partial total over
 * data that is now gone. The check is the one `write` makes of a buffer it is
 * about to hand a driver, `user_buffer_ok` with write access, so an address
 * that is merely inside the user window but unmapped or read-only is refused
 * here and not discovered half way through. A list that asks for nothing still
 * checks the descriptor, so a closed fd is -EBADF and not a quiet zero.
 *
 * The buffers are then filled in order, and what "in order" costs depends on
 * what is behind the descriptor, because a second read is not always the same
 * thing as reading on.
 *
 * A regular file is read again for each buffer. Its bytes carry no message
 * boundary, so a walk is the single scatter read.
 *
 * A datagram socket is NOT read again: one datagram is one message, and a
 * second read would take the next one while the first one's tail was dropped.
 * The waiting datagram is copied whole into a kernel buffer through
 * `socket_dgram_peek` and consumed at once, under the network lock, and the
 * buffer is then scattered piece by piece, so the whole of it fills as many
 * buffers as it needs and the remainder past the last one is discarded the
 * way a plain `read` discards it.
 *
 * A pipe, a stream socket, a device or a `/proc` file is read again only while
 * it says it has more, since a read of an empty one blocks for input nobody has
 * written. Asking and then reading is safe for two reasons together: no other
 * process runs in between, because a syscall is not preemptible here, and the
 * interrupts that do run can only ADD to what is readable, never take it away.
 * Nothing else empties a pipe or a receive ring.
 *
 * A terminal is read exactly once, and that is not a limitation but its
 * contract: the line discipline hands back one line, and filling a second
 * buffer with the line after it would return two lines where a reader asked
 * for one.
 *
 * Returning less than was asked for is what a reader must already handle, and
 * musl's stdio, which is what issues this call, loops.
 */
int32_t sys_readv(isr_params_t *params)
{
    int fd = (int)params->ebx;
    uint32_t iov_user = params->ecx;
    int iovcnt = (int)params->edx;
    struct kernel_iovec iov;
    fd_entry_t *entry;
    uint32_t wanted = 0;
    int32_t total = 0;
    int walk;
    int i;

    if (iovcnt < 0 || iovcnt > IOV_MAX_LOCAL) {
        return -EINVAL;
    }

    for (i = 0; i < iovcnt; i++) {
        if (copy_from_user(
                &iov, (const void __user *)(iov_user + (uint32_t)i * sizeof(struct kernel_iovec)),
                sizeof(iov)) != 0) {
            return -EFAULT;
        }
        if (iov.iov_len == 0) {
            continue;
        }
        if (iov.iov_len > 0x7FFFFFFFu || wanted > 0x7FFFFFFFu - iov.iov_len) {
            return -EINVAL;
        }
        if (!user_buffer_ok(iov.iov_base, iov.iov_len, 1)) {
            return -EFAULT;
        }
        wanted += iov.iov_len;
    }

    if (wanted == 0) {
        return read_fd(fd, (char __user *)0, 0);
    }

    entry = current_proc ? fd_get(current_proc->fds, fd) : (fd_entry_t *)0;
    if (entry && entry->type == FD_TYPE_SOCKET && entry->sock && entry->sock->type == SOCK_DGRAM) {
        return readv_dgram(entry, iov_user, iovcnt, 0, 0, 0, 0);
    }
    walk = entry && !entry->ops && entry->type == FD_TYPE_FILE;

    for (i = 0; i < iovcnt; i++) {
        int32_t r;

        if (copy_from_user(
                &iov, (const void __user *)(iov_user + (uint32_t)i * sizeof(struct kernel_iovec)),
                sizeof(iov)) != 0) {
            return (total > 0) ? total : -EFAULT;
        }
        if (iov.iov_len == 0) {
            continue;
        }
        if (!user_access_ok(iov.iov_base, iov.iov_len)) {
            return (total > 0) ? total : -EFAULT;
        }

        /* The first buffer is filled by the ordinary read, which waits if it
         * must; every buffer after it only by one that will not wait. */
        if (total > 0 && !walk) {
            r = read_ready(entry, fd, (char __user *)iov.iov_base, iov.iov_len);
            if (r <= 0) {
                break;
            }
        } else {
            r = read_fd(fd, (char __user *)iov.iov_base, iov.iov_len);
            if (r < 0) {
                return (total > 0) ? total : r;
            }
        }
        total += r;
        if ((uint32_t)r < iov.iov_len) {
            break;
        }
    }

    return total;
}

/**
 * Duplicate a descriptor onto the lowest free fd (dup(2)).
 *
 * EBX = oldfd. Returns the new fd, or -errno. The duplicate SHARES oldfd's
 * open-file description (shared seek offset); a socket gains a reference;
 * the new fd's FD_CLOEXEC is clear.
 */
int32_t sys_dup(isr_params_t *params)
{
    int oldfd = (int)params->ebx;
    fd_entry_t *entry;
    fd_entry_t *dst;
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
    /* The table may have grown, and its entries moved: the source is looked
     * up again. */
    entry = fd_get(current_proc->fds, oldfd);
    dst = &current_proc->fds->entries[newfd];
    fd_ofd_unref(dst->ofd); /* drop the fresh OFD; share oldfd's instead */
    dst->type = entry->type;
    dst->node = entry->node;
    dst->sock = entry->sock;
    dst->ops = entry->ops; /* a dup shares the same backing object + vtable */
    dst->priv = entry->priv;
    dst->dev = entry->dev;
    dst->ofd = entry->ofd;
    fd_ofd_ref(dst->ofd);
    dst->flags = entry->flags & ~(uint32_t)FD_FLAG_CLOEXEC;
    if (entry->type == FD_TYPE_SOCKET && entry->sock) {
        socket_ref(entry->sock);
    }
    return newfd;
}

/**
 * Free slot `fd` for dup2 and dup3 to put a copy in, closing the descriptor
 * there if there is one, with every backing it held (`fd_teardown`). -EBUSY
 * when the slot cannot be had: an open has reserved it and not yet filled
 * it, which is when Linux's dup2 answers EBUSY; or a call of another thread
 * sharing the table holds its descriptor, or the descriptor a close left to
 * such a call, or a close of it is still releasing it, where Linux replaces
 * the descriptor and the call or the close keeps its file.
 */
static int32_t dup_clear(int fd)
{
    fd_table_t *table = current_proc->fds;
    fd_entry_t *e = &table->entries[fd];

    if (e->type == FD_TYPE_NONE && !e->ofd) {
        return 0;
    }
    if (e->type == FD_TYPE_NONE || (e->flags & FD_FLAG_CLOSING) || fd_held_elsewhere(table, fd)) {
        return -EBUSY;
    }
    fd_close_locks(e);
    fd_teardown(table, fd);
    return e->type == FD_TYPE_NONE && !e->ofd ? 0 : -EBUSY;
}

/**
 * dup2(2): EBX = oldfd, ECX = newfd. Returns newfd or -errno. A `newfd`
 * past the end of the table grows it, and a heap with no room for that is
 * ENOMEM.
 *
 * The replacement of an open `newfd` may sleep (`dup_clear`), and another
 * thread may close `oldfd` meanwhile. The call holds `oldfd`, so the close
 * leaves it to the call, and the copy is made of what the call looked up, as
 * Linux's dup2 installs the file it looked up; the copy is an open
 * descriptor of its own, with none of the source's closing state.
 */
int32_t sys_dup2(isr_params_t *params)
{
    int oldfd = (int)params->ebx;
    int newfd = (int)params->ecx;
    fd_entry_t *old_entry;
    fd_entry_t *new_entry;
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
    old_entry = fd_get(current_proc->fds, oldfd); /* the entries may have moved */

    if (dup_clear(newfd) != 0) {
        return -EBUSY;
    }

    /* Duplicate the descriptor: it SHARES the open-file description (so the
     * seek offset is shared, per POSIX), gaining a reference; a duplicated
     * socket gains another reference too; and dup2 leaves the new fd's CLOEXEC
     * clear. */
    new_entry = &current_proc->fds->entries[newfd];
    new_entry->type = old_entry->type;
    new_entry->node = old_entry->node;
    new_entry->sock = old_entry->sock;
    new_entry->ops = old_entry->ops; /* share the backing object + vtable */
    new_entry->priv = old_entry->priv;
    new_entry->dev = old_entry->dev;
    new_entry->ofd = old_entry->ofd;
    fd_ofd_ref(new_entry->ofd);
    new_entry->flags =
        old_entry->flags & ~(uint32_t)(FD_FLAG_CLOEXEC | FD_FLAG_CLOSING | FD_FLAG_RELEASING);

    if (new_entry->type == FD_TYPE_SOCKET && new_entry->sock) {
        socket_ref(new_entry->sock);
    }

    return newfd;
}

/** Whether a synthetic node of `kind` is one of the directories. */
static int synth_kind_is_dir(int kind)
{
    return kind == VNODE_DEV_DIR || kind == VNODE_PTS_DIR || kind == VNODE_PROC_DIR ||
           kind == VNODE_PROC_NET_DIR || kind == VNODE_PROC_PID || kind == VNODE_PROC_FD_DIR ||
           kind == VNODE_PROC_TASK_DIR || kind == VNODE_PROC_SYS_DIR || kind == VNODE_SYS_DIR ||
           kind == VNODE_SYS_LINK;
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
 * Change the working directory to `path`, an absolute canonical path in a
 * path page, which this may write over.
 *
 * The working directory keeps a canonical path from the process's root
 * (`path_in_root`), so each kind of directory is named its own way: a
 * synthetic one by its kind, with /proc/self resolved to the caller's pid so
 * that a later relative name classifies right; one of another type as
 * `chdir_mounted` names it; one of the KRFFS boot root by its node. Entering
 * a directory takes search permission on it; anything else is -ENOTDIR. The
 * KRFFS root is a directory whatever its stored mode says: it is the file
 * system's anchor, not a node `mkdir` made. "/" is taken without a walk only
 * when it names the KRFFS boot root itself, which everyone may search; a
 * root chroot gave the process is resolved and checked like any other
 * directory.
 *
 * A type that keeps a node only while it is open, mounted KRFFS, counts no
 * hold for the walk's reference, and the checks between the walk and the
 * record may sleep for the disk. The node is pinned (`pin`) under the lock
 * the walk ends with, and the pin is let go once the working directory
 * records the node, so an rmdir that comes between finds the pin, and one
 * that comes after finds the working directory, and neither frees the
 * directory the record then names.
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
        int rpid = 0;

        vfs_ref_release(&ref);
        if (vp.kind == VNODE_PROC_PID || vp.kind == VNODE_PROC_FD_DIR ||
            vp.kind == VNODE_PROC_TASK_DIR) {
            if (procfs_resolve_id(&vp) < 0) {
                return -ENOENT;
            }
            rpid = procfs_resolve_pid(vp.pid);
        }
        if (!synth_kind_is_dir(vp.kind)) {
            return -ENOTDIR;
        }
        if (vp.kind == VNODE_SYS_DIR || vp.kind == VNODE_SYS_LINK) {
            if (SYSFS_KEYED(vp.sysent) && !sysfs_device_exists(vp.sysent, vp.syskey)) {
                return -ENOENT;
            }
            synth_sys_path(vp.sysent, vp.syskey, path, VFS_PATH_MAX);
        } else {
            synth_dir_path(vp.kind, vp.kind == VNODE_PROC_SYS_DIR ? vp.sysent : rpid, vp.tid, path,
                           VFS_PATH_MAX);
        }
        return cwd_enter(path, NULL, NULL);
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
 * Change the current working directory (chdir(2)).
 *
 * EBX = path. Returns 0 or -errno. Stores the canonical absolute path of the
 * resolved directory (a KRFFS directory, a directory of another mounted
 * file system, or a synthetic /dev or /proc directory); a regular file is
 * -ENOTDIR, a missing path -ENOENT.
 */
int32_t sys_chdir(isr_params_t *params)
{
    const char __user *upath = (const char __user *)params->ebx;
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
 * fchdir(2): EBX = fd. Make the directory `fd` is open on the working
 * directory.
 *
 * The working directory's path is the one the descriptor's `*at` calls
 * resolve against (`fd_dir_path`), so it follows a directory a type can name
 * after a rename; a descriptor on anything but a directory is -ENOTDIR, and
 * one on a directory tmpfs removed since is -ENOENT. Entering takes search
 * permission on the directory, which a synthetic one grants everybody, and
 * the mount hold chdir takes. A descriptor opened with O_PATH does as well as
 * any other.
 */
int32_t sys_fchdir(isr_params_t *params)
{
    fd_entry_t *entry;
    struct vfs_ref ref;
    char *path;
    int32_t rc;

    if (!current_proc) {
        return -ESRCH;
    }
    entry = fd_get(current_proc->fds, (int)params->ebx);
    if (!entry) {
        return -EBADF;
    }
    path = path_buf_get();
    if (!path) {
        return -ENOMEM;
    }
    rc = fd_dir_path(entry, path, VFS_PATH_MAX);
    if (rc == 0 && entry->type == FD_TYPE_SYNTH_DIR) {
        rc = cwd_enter(path, NULL, NULL);
    } else if (rc == 0 && (!entry->ofd || !entry->ofd->vmnt)) {
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
 * search, and a caller who is root, checked in that order, as Linux checks
 * them. A directory of /dev or /proc is EINVAL: those types take the rest
 * of a path in one call, and the resolver could not stop `..` inside it.
 * Returns 0, -ENOTDIR, -EACCES, -EPERM or -EINVAL.
 */
static int32_t root_check(const struct vfs_ref *ref)
{
    const struct vpath *vp = vfs_synth_vpath(ref->mnt, ref->node);
    const krffs_node_t *node = ref->node;
    struct vfs_stat vst;
    int32_t rc = 0;

    if (vp) {
        rc = synth_kind_is_dir(vp->kind) ? 0 : -ENOTDIR;
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
    if (rc == 0 && !cred_privileged()) {
        rc = -EPERM;
    }
    return rc == 0 && vp ? -EINVAL : rc;
}

/**
 * The working directory's path from the root of the mount table, into `buf`
 * of VFS_PATH_MAX bytes: its path from the calling process's root, behind
 * the path of that root when chroot gave the process one.
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
 * last, after the old root is let go.
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
 * chroot(2): EBX = path. Make the directory `path` names the calling
 * process's root: its absolute paths and the absolute targets of its
 * symbolic links start there, and `..` there stays there (vfs.Readme.md).
 *
 * The refusals come in Linux's order: the path's own (ENOENT, ENOTDIR,
 * ELOOP, ENAMETOOLONG, EACCES on the way), then `root_check`'s. The working
 * directory stays where it is when it is inside the new root, and moves to
 * the new root when it is outside, since it is a path from the process's
 * root; Linux leaves it where it was either way. fork passes the root on
 * and exec keeps it. While a process has it, rmdir refuses the directory
 * with EBUSY and the root holds its mount against unmount.
 */
int32_t sys_chroot(isr_params_t *params)
{
    const char __user *upath = (const char __user *)params->ebx;
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
        if (rc == 0) {
            rc = root_enter(&ref, path);
        }
        vfs_ref_release(&ref);
    }
    path_buf_put(path);
    return rc;
}

/**
 * getcwd(2): EBX = buf, ECX = size. Returns the path's length with its NUL,
 * as Linux's system call does, or -errno.
 *
 * The path is copied into a path page before the copy out, which may sleep
 * on a fault: a thread that shares the working directory (CLONE_FS) may
 * chdir meanwhile, which frees a long path's string. -ENOMEM when no page is
 * to be had.
 */
int32_t sys_getcwd(isr_params_t *params)
{
    char __user *buf = (char __user *)params->ebx;
    uint32_t size = params->ecx;
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
    if (cwd_len + 1 > size) {
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
 * Linux 32-bit stat structure (used by syscall 106)
 * This is the older structure used by ISH and some programs.
 */
struct linux_stat {
    unsigned long st_dev;
    unsigned long st_ino;
    unsigned short st_mode;
    unsigned short st_nlink;
    unsigned short st_uid;
    unsigned short st_gid;
    unsigned long st_rdev;
    unsigned long st_size;
    unsigned long st_blksize;
    unsigned long st_blocks;
    unsigned long st_atime;
    unsigned long st_atime_nsec;
    unsigned long st_mtime;
    unsigned long st_mtime_nsec;
    unsigned long st_ctime;
    unsigned long st_ctime_nsec;
    unsigned long __unused4;
    unsigned long __unused5;
};

/*
 * Linux stat64 structure for 32-bit systems
 */
struct linux_stat64 {
    unsigned long long st_dev;
    unsigned char __pad0[4];
    unsigned long __st_ino;
    unsigned int st_mode;
    unsigned int st_nlink;
    unsigned long st_uid;
    unsigned long st_gid;
    unsigned long long st_rdev;
    unsigned char __pad3[4];
    long long st_size;
    unsigned long st_blksize;
    unsigned long long st_blocks;
    unsigned long st_atime;
    unsigned long st_atime_nsec;
    unsigned long st_mtime;
    unsigned long st_mtime_nsec;
    unsigned long st_ctime;
    unsigned long st_ctime_nsec;
    unsigned long long st_ino;
};

/**
 * Report a node's three times into a stat64.
 *
 * KRFFS stores them; before this kernel had a clock nothing read them out, so
 * every file reported the epoch and no build tool could tell a source from the
 * object built from it. Access time is REPORTED but never advanced by a read
 * (see fs/krffs.c): the file system is a write-back page cache, and stamping an
 * atime on every read would dirty a page on every read.
 */
static void stat64_node_times(struct linux_stat64 *kst, const krffs_node_t *node)
{
    kst->st_atime = (unsigned long)node->access_time.seconds;
    kst->st_atime_nsec = (unsigned long)node->access_time.nanoseconds;
    kst->st_mtime = (unsigned long)node->modification_time.seconds;
    kst->st_mtime_nsec = (unsigned long)node->modification_time.nanoseconds;
    kst->st_ctime = (unsigned long)node->attribute_change_time.seconds;
    kst->st_ctime_nsec = (unsigned long)node->attribute_change_time.nanoseconds;
}

/**
 * Report the boot time into a stat64, for a node with no history of its own: a
 * device, a /proc file, a pipe, a socket.
 */
static void stat64_boot_times(struct linux_stat64 *kst)
{
    unsigned long boot = (unsigned long)rtc_boot_time();

    kst->st_atime = boot;
    kst->st_mtime = boot;
    kst->st_ctime = boot;
    kst->st_atime_nsec = 0;
    kst->st_mtime_nsec = 0;
    kst->st_ctime_nsec = 0;
}

/** Fill the common identity of a read-only global proc file. */
static int32_t synth_proc_stat(struct linux_stat64 *kst, uint32_t inode)
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
static void synth_sys_identity(const struct vpath *vp, struct linux_stat64 *kst, int follow)
{
    kst->st_dev = SYSFS_FSID;
    if (vp->kind == VNODE_SYS_FILE) {
        kst->st_mode = S_IFREG | 0444;
        kst->st_size = 4096;
        kst->st_ino = sysfs_ino(vp->sysent, vp->syskey, 1 + vp->procfile);
    } else if (vp->kind == VNODE_SYS_LINK && !follow) {
        kst->st_mode = 0120777; /* S_IFLNK | 0777 */
        kst->st_ino = sysfs_ino(vp->sysent, vp->syskey, 2);
    } else {
        kst->st_mode = S_IFDIR | 0555;
        kst->st_ino = sysfs_ino(vp->sysent, vp->syskey, 0);
    }
}

/** The identity of a node of /sys, as `synth_sys_identity` gives it, or
 * -ENOENT for a device that is not there. */
static int32_t synth_sys_stat(const struct vpath *vp, struct linux_stat64 *kst, int follow)
{
    if (SYSFS_KEYED(vp->sysent) && !sysfs_device_exists(vp->sysent, vp->syskey)) {
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
    char path[64];
    const char *up = "../../..";
    int n = 0, i;

    synth_sys_path(vp->sysent, vp->syskey, path, (int)sizeof(path));
    for (i = 0; up[i] != '\0' && n < cap; i++) {
        out[n++] = up[i];
    }
    for (i = 4; path[i] != '\0' && n < cap; i++) {
        out[n++] = path[i];
    }
    return n;
}

/**
 * The root directory's own three times, into a stat64.
 *
 * The root is a real node and creating an entry in it stamps its times, so it
 * reports them like any other directory; a machine with no file system mounted
 * has nothing to report but the boot time.
 */
static void stat64_root_times(struct linux_stat64 *kst)
{
    if (krffs_fs && krffs_fs->node) {
        stat64_node_times(kst, krffs_fs->node);
    } else {
        stat64_boot_times(kst);
    }
}

/* A stat64 for a node on a mount that is not the root, from its getattr:
 * the mount's index is the device, so a program telling files apart by
 * (st_dev, st_ino) sees two mounts as two devices. */
static int32_t vfs_fill_stat64(struct linux_stat64 *kst, const struct vfs_ref *r)
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
    for (i = 0; i < vfs_mount_slots(); i++) {
        if (vfs_mount_at(i) == r->mnt) {
            kst->st_dev = (unsigned long long)(0x10u + (unsigned)i);
        }
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
    kst->__st_ino = (unsigned long)st.ino;
    kst->st_atime = (long)st.atime_sec;
    kst->st_atime_nsec = (unsigned long)st.atime_nsec;
    kst->st_mtime = (long)st.mtime_sec;
    kst->st_mtime_nsec = (unsigned long)st.mtime_nsec;
    kst->st_ctime = (long)st.ctime_sec;
    kst->st_ctime_nsec = (unsigned long)st.ctime_nsec;
    return 0;
}

static void krffs_fill_stat64(struct linux_stat64 *kst, krffs_node_t *node)
{
    k_memset(kst, 0, sizeof(*kst));
    if (node == krffs_fs->node) {
        /* The root is a directory even if its on-disk mode lacks S_IFDIR (it is
         * the filesystem anchor); a path can reach it via "..". */
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
    kst->__st_ino = (unsigned long)kst->st_ino;
    stat64_node_times(kst, node);
}

/* Fill a stat64 from an open descriptor (factored from sys_fstat64 so the magic
 * /proc/<pid>/fd/<n> symlinks can stat their target when followed). */
static int32_t fstat_fill(fd_entry_t *e, struct linux_stat64 *kst);

/**
 * What a followed /proc/PID/fd/N link reports: the stat of descriptor `fd`
 * of process `pid`, when it is open and not closed; -ENOENT otherwise. The
 * entry is copied, and a description of a file or a directory on a mount is
 * held for the length of the stat, whose getattr may sleep while the
 * process closes the descriptor and its release puts the node. The other
 * kinds are read without a sleep.
 */
static int32_t synth_fd_stat(int pid, int fd, struct linux_stat64 *kst)
{
    process_t *p = get_process((uint32_t)pid);
    fd_entry_t e;
    int held;

    if (!p || fd < 0 || fd >= fd_table_cap(p->fds) || p->fds->entries[fd].type == FD_TYPE_NONE ||
        (p->fds->entries[fd].flags & FD_FLAG_CLOSING)) {
        return -ENOENT;
    }
    e = p->fds->entries[fd];
    held = (e.type == FD_TYPE_FILE || e.type == FD_TYPE_DIR) && e.ofd && e.ofd->vmnt;
    if (held) {
        fd_ofd_ref(e.ofd);
    }
    fstat_fill(&e, kst);
    if (held) {
        fd_ofd_unref(e.ofd);
    }
    return 0;
}

/* Stable /dev/pts inodes (distinct from devfs's): the directory, then one per
 * slave (so a slave's (st_dev, st_ino) identity is consistent for ttyname). */
#define PTS_INO_DIR 3u
/* /dev/log's inode, below the slaves' and above the directory's. */
#define DEV_LOG_INO 4u

#define PTS_INO_BASE 0x1000u

/** Give `kst` the owner of process `pid`'s files under /proc, as Linux's
 * `task_dump_owner` does: the process's effective user and group IDs while
 * it is dumpable, and root otherwise (`procfs_oom_owner`). BusyBox's `ps`
 * and `top` read a process's user from its directory's owner. */
static void synth_pid_owner(int pid, struct linux_stat64 *kst)
{
    uint32_t uid, gid;

    if (procfs_oom_owner(pid, &uid, &gid) == 0) {
        kst->st_uid = uid;
        kst->st_gid = gid;
    }
}

static int32_t synth_stat(const struct vpath *vp, struct linux_stat64 *kst, int follow)
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
        if (devfs_stat(vp->name, &mode, &rdev, &ino) != 0) {
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
        /* /proc/cmdline: the boot command line, a read-only regular
         * file. */
        kst->st_dev = PROCFS_FSID;
        kst->st_mode = S_IFREG | 0444;
        kst->st_ino = procfs_ino_cmdline();
        return 0;
    case VNODE_PROC_NET_DIR:
        kst->st_dev = PROCFS_FSID;
        kst->st_mode = S_IFDIR | 0555;
        kst->st_ino = procfs_ino_netdir();
        return 0;
    case VNODE_PROC_NET_DEV:
        /* /proc/net/dev: what the interface has carried, anyone's to
         * read. */
        kst->st_dev = PROCFS_FSID;
        kst->st_mode = S_IFREG | 0444;
        kst->st_ino = procfs_ino_netdev();
        return 0;
    case VNODE_PROC_UPTIME:
        kst->st_dev = PROCFS_FSID;
        kst->st_mode = S_IFREG | 0444;
        kst->st_ino = procfs_ino_uptime();
        return 0;
    case VNODE_PROC_STAT_SYS:
        kst->st_dev = PROCFS_FSID;
        kst->st_mode = S_IFREG | 0444;
        kst->st_ino = procfs_ino_statsys();
        return 0;
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
        kst->st_dev = PROCFS_FSID;
        kst->st_mode = S_IFREG | 0444;
        kst->st_ino = procfs_ino_filesystems();
        return 0;
    case VNODE_PROC_INTERRUPTS:
        kst->st_dev = PROCFS_FSID;
        kst->st_mode = S_IFREG | 0444;
        kst->st_ino = procfs_ino_interrupts();
        return 0;
    case VNODE_PROC_CONSOLE:
        kst->st_dev = PROCFS_FSID;
        kst->st_mode = S_IFREG | 0444;
        kst->st_ino = procfs_ino_console();
        return 0;
    case VNODE_PROC_NET_ROUTE:
        kst->st_dev = PROCFS_FSID;
        kst->st_mode = S_IFREG | 0444;
        kst->st_ino = procfs_ino_netroute();
        return 0;
    case VNODE_PROC_NET_ARP:
        kst->st_dev = PROCFS_FSID;
        kst->st_mode = S_IFREG | 0444;
        kst->st_ino = procfs_ino_netarp();
        return 0;
    case VNODE_PROC_NET_TCP:
        kst->st_dev = PROCFS_FSID;
        kst->st_mode = S_IFREG | 0444;
        kst->st_ino = procfs_ino_nettcp();
        return 0;
    case VNODE_PROC_NET_FILE:
        return synth_proc_stat(kst, procfs_ino_net(vp->procfile));
    case VNODE_SYS_DIR:
    case VNODE_SYS_LINK:
    case VNODE_SYS_FILE:
        return synth_sys_stat(vp, kst, follow);
    case VNODE_PROC_DISK:
        /* /proc/disk: what the disk reported, root-readable only. */
        kst->st_dev = PROCFS_FSID;
        kst->st_mode = S_IFREG | 0400;
        kst->st_ino = procfs_ino_disk();
        return 0;
    case VNODE_PROC_CWD:
    case VNODE_PROC_ROOT:
    case VNODE_PROC_EXE: {
        char scratch[1];
        int rpid = procfs_resolve_id(vp);

        /* Only an un-followed stat comes here. A link the caller may not
         * read exists, as on Linux; one of no program or no process does
         * not. */
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
    case VNODE_PROC_PID:
    case VNODE_PROC_FD_DIR:
    case VNODE_PROC_TASK_DIR:
        if (procfs_resolve_id(vp) < 0) {
            return -ENOENT;
        }
        /* /proc/self is a symlink: an un-followed stat (lstat) sees the
         * link, a followed one resolves to the pid directory.
         * /proc/<numeric> and /proc/<pid>/fd are real directories. */
        if (vp->kind == VNODE_PROC_PID && vp->pid == SYNTH_PID_SELF && vp->tid == 0 && !follow) {
            kst->st_dev = PROCFS_FSID;
            kst->st_mode = 0120777; /* S_IFLNK | 0777 */
            kst->st_ino = procfs_ino_dir();
            return 0;
        }
        kst->st_dev = PROCFS_FSID;
        kst->st_mode = S_IFDIR | 0555;
        kst->st_ino = procfs_ino_pid(procfs_resolve_id(vp));
        synth_pid_owner(procfs_resolve_id(vp), kst);
        if (vp->kind == VNODE_PROC_TASK_DIR) {
            kst->st_nlink = procfs_task_links(procfs_resolve_id(vp));
        }
        return 0;
    case VNODE_PROC_FILE:
        if (procfs_resolve_id(vp) < 0) {
            return -ENOENT;
        }
        kst->st_dev = PROCFS_FSID;
        kst->st_mode = S_IFREG | 0444;
        kst->st_ino = procfs_ino_pid(procfs_resolve_id(vp));
        synth_pid_owner(procfs_resolve_id(vp), kst);
        /* oom_score_adj is its owner's to write, as on Linux. */
        if (vp->procfile == SYNTH_PROC_OOM_ADJ) {
            kst->st_mode = S_IFREG | 0644;
        }
        return 0;
    case VNODE_PROC_FD_LINK: {
        char scratch[16];
        int rpid = procfs_resolve_id(vp);
        if (rpid < 0) {
            return -ENOENT;
        }
        /* The link exists only if descriptor procfd is actually open (the
         * same check readlink makes), so stat and readlink agree. */
        if (procfs_readlink_fd(rpid, vp->procfd, scratch, (int)sizeof(scratch)) < 0) {
            return -ENOENT;
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
        /* /dev/pts: a directory. */
        kst->st_dev = DEVFS_FSID;
        kst->st_mode = S_IFDIR | 0755;
        kst->st_ino = PTS_INO_DIR;
        return 0;
    case VNODE_PTS: {
        /* /dev/pts/N: a slave terminal (S_IFCHR), only if pty N is
         * allocated. The rdev is the Linux Unix98 pty slave major 136.
         * Ownership lives on the pty, where the login server's chown/chmod
         * put it. */
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
        return 0;
    }
    case VNODE_ENOTDIR:
        return -ENOTDIR;
    default:
        return -ENOENT;
    }
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
    struct linux_stat64 kst;
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
 * `vfs_resolve` of `name` with the flags `walk`, for a call whose path named
 * a directory by its end when `dir` is not 0 (`path_names_dir`): such a
 * path follows a final symbolic link whatever `walk` says, and must reach a
 * directory, as Linux's walk requires, or the call is -ENOTDIR, with nothing
 * held and no lock kept. Returns 0 with `ref` held, and the final mount
 * locked under VFS_WALK_LOCK, or a negative errno.
 *
 * The node checked is the node returned, so a call that goes on with `ref`
 * acts on what was checked; execve opens its program and interpreters
 * through it (`exec_open` in `syscall_proc.c`), since a second walk could
 * meet a symbolic link replaced in between.
 */
int32_t resolve_named(const char *name, int walk, int dir, struct vfs_ref *ref)
{
    int32_t rc = vfs_resolve(0, name, dir ? walk & ~VFS_WALK_NOFOLLOW : walk, ref, 0, 0);

    if (rc == 0 && dir) {
        rc = ref_dir_check(ref, 1);
        if (rc != 0) {
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
 * `vp` for fstat and fstatfs and `name` for /proc/PID/fd. Nothing is opened:
 * a device does not learn of it and a /proc file takes no snapshot.
 *
 * The /proc links are classified, not resolved, so the O_NOFOLLOW choice is
 * made here: /proc/self is the process directory unless O_NOFOLLOW names the
 * link, and a /proc/PID/fd link is -ENOENT unless O_NOFOLLOW, as open(2)
 * answers for one; Linux follows it to the descriptor's file. O_DIRECTORY on
 * anything but a directory is -ENOTDIR, and a node that is gone, a process
 * that exited among them, is -ENOENT.
 */
static int32_t synth_open_path(const struct vpath *vp, const char *name, int flags)
{
    int follow = !(flags & O_NOFOLLOW);
    struct linux_stat64 kst;
    struct vpath *copy;
    fd_entry_t *e;
    int fd;

    if (follow && vp->kind == VNODE_PROC_FD_LINK) {
        return -ENOENT;
    }
    if (synth_stat(vp, &kst, follow) != 0) {
        return -ENOENT;
    }
    if ((kst.st_mode & S_IFMT) == S_IFDIR) {
        fd = synth_open_dir(vp, flags & O_CLOEXEC);
        if (fd >= 0) {
            e = &current_proc->fds->entries[fd];
            e->flags = (e->flags & FD_FLAG_CLOEXEC) | FD_FLAG_PATH;
        }
        return fd;
    }
    if (flags & O_DIRECTORY) {
        return -ENOTDIR;
    }
    copy = kmalloc(sizeof(*copy));
    if (!copy) {
        return -ENOMEM;
    }
    fd = fd_alloc(current_proc->fds);
    if (fd < 0) {
        kfree(copy);
        return fd;
    }
    *copy = *vp;
    e = &current_proc->fds->entries[fd];
    e->type = FD_TYPE_SYNTH_PATH;
    e->node = NULL;
    e->sock = NULL;
    e->ops = NULL;
    e->priv = copy;
    e->dev = NULL;
    ofd_record_path(e->ofd, name);
    e->flags = FD_FLAG_PATH | ((flags & O_CLOEXEC) ? FD_FLAG_CLOEXEC : 0);
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
    entry = &current_proc->fds->entries[fd];
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
    entry->ofd->dirpath[0] = '\0';
    if (!mnt_is_root(ref->mnt)) {
        ofd_record_path(entry->ofd, name);
    }
    entry->ofd->vmnt = ref->mnt;
    entry->ofd->vnode = ref->node;
    entry->ofd->fhandle = NULL;
    vfs_desc_open(ref->mnt);
    vfs_leave(ref->mnt);
    entry->flags = FD_FLAG_PATH | ((flags & O_CLOEXEC) ? FD_FLAG_CLOEXEC : 0);
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
    rc = vfs_resolve(0, name, walk, &ref, 0, 0);
    if (rc != 0) {
        return rc;
    }
    locked = ref.mnt;
    vp = vfs_synth_vpath(ref.mnt, ref.node);
    if (vp) {
        struct vpath vpc = *vp;

        vfs_walk_unlock(locked);
        vfs_ref_release(&ref);
        return synth_open_path(&vpc, name, flags);
    }
    rc = path_install(&ref, name, flags);
    vfs_walk_unlock(locked);
    return rc;
}

/**
 * readlink core (readlink(2)/readlinkat(2)).
 *
 * The links are /proc/self, the /proc/<pid>/fd/<n> magic links (which
 * ttyname reads), and the symbolic links of a mounted type that has them,
 * read through the type. The result is the byte count and is NOT
 * NUL-terminated (the readlink contract; musl's readlink passes the count
 * through, and its ttyname_r is what appends a terminator and treats a full
 * buffer as truncation). `name` is the path, leading '/' stripped. A path
 * that named a directory by its end, `dir`, follows a final link and must
 * reach a directory (`resolve_named`), which is no link: -EINVAL, as on
 * Linux.
 */
static int32_t do_readlink(const char *name, char __user *ubuf, uint32_t bufsiz, int dir)
{
    struct vpath vp;
    struct vfs_ref ref;
    const struct vpath *svp;
    char *link; /* a target up to the resolver's own bound, in a path page */
    int n, pid, kind;

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
    if (!svp) {
        /* A link on a mounted type: its target from the type, copied out
         * without the terminator, as readlink(2) does. */
        n = ref.mnt->type->ops->readlink
                ? ref.mnt->type->ops->readlink(ref.mnt, ref.node, link, VFS_PATH_MAX)
                : -EINVAL;
        vfs_ref_release(&ref);
        goto out;
    }
    vp = *svp;
    vfs_ref_release(&ref);
    kind = vp.kind;

    if (kind == VNODE_PROC_PID && vp.pid == SYNTH_PID_SELF && vp.tid == 0) {
        /* /proc/self is a symlink to the caller's numeric pid. */
        uint32_t uv;
        char t[12];
        int tn = 0;
        pid = procfs_resolve_pid(vp.pid);
        n = 0;
        uv = (uint32_t)pid;
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
        pid = procfs_resolve_id(&vp);
        n = pid < 0 ? -ENOENT : procfs_readlink_fd(pid, vp.procfd, link, VFS_PATH_MAX);
    } else if (kind == VNODE_PROC_EXE) {
        pid = procfs_resolve_id(&vp);
        n = pid < 0 ? -ENOENT : procfs_readlink_exe(pid, link, VFS_PATH_MAX);
    } else if (kind == VNODE_PROC_CWD || kind == VNODE_PROC_ROOT) {
        pid = procfs_resolve_id(&vp);
        n = pid < 0 ? -ENOENT
                    : procfs_readlink_dir(pid, kind == VNODE_PROC_ROOT, link, VFS_PATH_MAX);
    } else if (kind == VNODE_SYS_LINK) {
        n = sysfs_device_exists(vp.sysent, vp.syskey) ? synth_sys_link(&vp, link, VFS_PATH_MAX)
                                                      : -ENOENT;
    } else if (kind == VNODE_DEV_LINK) {
        n = (int)k_strlen(SYNTH_DEV_LOG_TARGET);
        k_memcpy(link, SYNTH_DEV_LOG_TARGET, (uint32_t)n);
    } else {
        n = -EINVAL; /* not a symlink */
    }

out:
    if (n >= 0 && (uint32_t)n > bufsiz) {
        n = (int)bufsiz; /* truncate to the caller's buffer */
    }
    if (n >= 0 && copy_to_user(ubuf, link, (uint32_t)n) != 0) {
        n = -EFAULT;
    }
    path_buf_put(link);
    return n;
}

int32_t sys_readlink(isr_params_t *params)
{
    const char __user *upath = (const char __user *)params->ebx;
    char __user *ubuf = (char __user *)params->ecx;
    uint32_t bufsiz = params->edx;
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
    rc = do_readlink(path_name(path), ubuf, bufsiz, dir);
    path_buf_put(path);
    return rc;
}

int32_t sys_readlinkat(isr_params_t *params)
{
    int dirfd = (int)params->ebx;
    const char __user *upath = (const char __user *)params->ecx;
    char __user *ubuf = (char __user *)params->edx;
    uint32_t bufsiz = params->esi;
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
    rc = do_readlink(path_name(path), ubuf, bufsiz, dir);
    path_buf_put(path);
    return rc;
}

/* Whether a synthetic node is one of /proc's links, by its unfollowed mode. */
static int synth_is_link(const struct vpath *vp)
{
    struct linux_stat64 lst;

    return synth_stat(vp, &lst, 0) == 0 && (lst.st_mode & 0170000u) == 0120000u;
}

/**
 * The attributes stat64 and lstat64 report for the user's `path`, a final
 * symbolic link followed when `follow`, copied out to `st`.
 *
 * The walk takes the lock of the mount it ends on, where the type asks for
 * one, and keeps it until the attributes are read: a node of a mounted
 * KRFFS volume is its offset, which a growing write that held the lock would
 * move, and a read at the offset the walk found would report whatever the
 * volume put there since.
 */
static int32_t do_stat64(const char __user *path, struct linux_stat64 __user *st, int follow)
{
    struct linux_stat64 kst;
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

    /* Build the result in a kernel struct, then copy it out in one shot. */
    k_memset(&kst, 0, sizeof(kst));

    {
        struct vfs_ref ref;
        const struct vpath *vp;
        int rc =
            vfs_resolve(0, pathbuf, VFS_WALK_LOCK | (follow ? 0 : VFS_WALK_NOFOLLOW), &ref, 0, 0);

        path_buf_put(pathbuf);
        if (rc != 0) {
            return rc; /* missing, not-a-dir, or a denied ancestor */
        }
        vp = vfs_synth_vpath(ref.mnt, ref.node);
        if (vp) {
            rc = synth_stat(vp, &kst, follow);
        } else if (!mnt_is_root(ref.mnt)) {
            rc = vfs_fill_stat64(&kst, &ref);
        } else if ((krffs_node_t *)ref.node == krffs_fs->node) {
            /* The root's identity is fixed: ino 1, 0755, and real times: an
             * entry created in it stamps them, which is how a program
             * watching a directory sees it. */
            kst.st_mode = S_IFDIR | 0755;
            kst.st_nlink = 1;
            kst.st_ino = kst.__st_ino = 1;
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
    if (copy_to_user(st, &kst, sizeof(kst)) != 0) {
        return -EFAULT;
    }

    return 0;
}

/* stat64(195) follows symlinks; lstat64(196) does not. On i386 musl maps its
 * stat()/lstat() onto these (via the statx -> *stat64 fallback), so lstat()
 * only works once 196 exists. The follow flag decides whether a symbolic
 * link at the end, on /proc or on a mounted type, is the result or its
 * target; a link the path names as a directory by its end is followed
 * either way (`path_names_dir`), and such a path that reaches anything but a
 * directory is -ENOTDIR. */
int32_t sys_stat64(isr_params_t *params)
{
    return do_stat64((const char __user *)params->ebx, (struct linux_stat64 __user *)params->ecx,
                     1);
}

int32_t sys_lstat64(isr_params_t *params)
{
    return do_stat64((const char __user *)params->ebx, (struct linux_stat64 __user *)params->ecx,
                     0);
}

/*
 * The i386 struct statfs64, as Linux writes it and musl reads it.
 *
 * `f_bsize` is the unit `f_blocks` and `f_bfree` are counted in. KRFFS has no
 * block structure at all, so the unit is chosen rather than found: one byte
 * would overflow the 64-bit counts on no disk this machine can hold, and 1024
 * is what `df` prints in by default, so the numbers a person reads are the
 * numbers the kernel holds divided by nothing.
 */
struct linux_statfs64 {
    uint32_t f_type;
    uint32_t f_bsize;
    uint64_t f_blocks;
    uint64_t f_bfree;
    uint64_t f_bavail;
    uint64_t f_files;
    uint64_t f_ffree;
    uint32_t f_fsid[2];
    uint32_t f_namelen;
    uint32_t f_frsize;
    uint32_t f_flags;
    uint32_t f_spare[4];
};

#define STATFS_BSIZE 1024u

/* The three permanent mounts and the magic each reports; `/proc/mounts` names
 * these, the tmpfs at /dev/shm, which answers with its own statfs as any
 * mounted volume does, and every volume `mount(2)` has placed. The values are
 * Linux's for the two synthetic ones, so a program that recognises them finds
 * what it expects; KRFFS has no assigned magic and uses its own name. */
#define STATFS_MAGIC_KRFFS 0x4B524646u /* "KRFF" */

#define STATFS_MAGIC_PROC 0x9FA0u

#define STATFS_MAGIC_DEVFS 0x1373u
#define STATFS_MAGIC_SYSFS 0x62656572u

/**
 * Fill a statfs64 for the boot root or one of the two synthetic mounts;
 * `statfs_fill_mount` answers for a mounted volume from its own statfs.
 *
 * Only the root has blocks: it is KRFFS, and the region's size and its free
 * nodes' usable bytes are both real, reported in 1024-byte units because that
 * is what `df` prints in by default. The two synthetic mounts report their own
 * magic and no blocks at all, which is what they have and what Linux reports
 * for them; answering with the root's numbers would tell `df` that `/proc` is
 * a 16 MB file system 77 percent full. `f_files` is zero throughout, which says
 * "not counted" rather than claiming an inode budget this format does not keep.
 */
static int32_t statfs_fill(struct linux_statfs64 __user *out, uint32_t magic)
{
    struct linux_statfs64 st;
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

/** A statfs64 from a mount's own statfs, whichever type answered it. */
static int32_t statfs_fill_mount(struct linux_statfs64 __user *out, struct vfs_mount *m)
{
    struct linux_statfs64 st;
    struct vfs_statfs vs;
    int rc = vfs_statfs(m, &vs);

    if (rc != 0) {
        return rc;
    }
    k_memset(&st, 0, sizeof(st));
    st.f_type = vs.magic;
    st.f_bsize = vs.bsize;
    st.f_frsize = vs.bsize;
    st.f_blocks = vs.blocks;
    st.f_bfree = vs.bfree;
    st.f_bavail = vs.bfree > vs.reserved ? vs.bfree - vs.reserved : 0;
    st.f_files = vs.files;
    st.f_ffree = vs.ffree;
    st.f_namelen = vs.namelen;
    if (copy_to_user(out, &st, sizeof(st)) != 0) {
        return -EFAULT;
    }
    return 0;
}

/** Which of the three mounts a classified path belongs to. */
static uint32_t statfs_magic_for_kind(int kind)
{
    if (kind == VNODE_DEV_DIR || kind == VNODE_DEV || kind == VNODE_PTS_DIR || kind == VNODE_PTS ||
        kind == VNODE_DEV_LINK) {
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

/**
 * statfs64(2): the file system a path is on.
 *
 * EBX is the path, ECX the caller's sizeof(struct statfs64), EDX the buffer.
 * The size is checked rather than ignored: a caller built against a different
 * layout would otherwise be handed bytes it would read as different fields.
 * A path that names a directory by its end must reach one (`resolve_named`).
 */
int32_t sys_statfs64(isr_params_t *params)
{
    char *pathbuf;
    int len;
    int dir;

    if (!current_proc || !params->ebx || !params->edx) {
        return -EFAULT;
    }
    if (params->ecx != sizeof(struct linux_statfs64)) {
        return -EINVAL;
    }
    len = path_from_user_dir(AT_FDCWD, (const char __user *)params->ebx, &pathbuf, &dir);
    if (len != 0) {
        return len;
    }
    /* The path has to name something. Answering for a path that does not exist
     * would report a file system where there is no file, which is the one thing
     * a caller uses this call to find out. */
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
            return statfs_fill_mount((struct linux_statfs64 __user *)params->edx, m);
        }
        vfs_ref_release(&ref);
        return statfs_fill((struct linux_statfs64 __user *)params->edx, magic);
    }
}

/**
 * fstatfs64(2): the same numbers, named by an open descriptor; a /proc
 * file's descriptor answers proc's, and a device's or a synthetic
 * directory's, /proc and /proc/PID among them, answers devfs's.
 *
 * EBX is the descriptor, ECX the caller's size, EDX the buffer.
 */
int32_t sys_fstatfs64(isr_params_t *params)
{
    fd_entry_t *e;

    if (params->ecx != sizeof(struct linux_statfs64)) {
        return -EINVAL;
    }
    if (!current_proc || !params->edx) {
        return -EFAULT;
    }
    e = fd_get(current_proc->fds, (int)params->ebx);
    if (!e) {
        return -EBADF;
    }
    /* The descriptor's own kind decides which mount answers. A socket or a pipe
     * is on no file system at all, and Linux answers `-ENOSYS` for those rather
     * than handing back the root's numbers. */
    if (e->type == FD_TYPE_PROC) {
        return statfs_fill((struct linux_statfs64 __user *)params->edx,
                           procfs_fd_is_sys(e) ? STATFS_MAGIC_SYSFS : STATFS_MAGIC_PROC);
    }
    if (e->type == FD_TYPE_SYNTH_DIR && e->priv &&
        ((const struct synth_dir *)e->priv)->kind == VNODE_SYS_DIR) {
        return statfs_fill((struct linux_statfs64 __user *)params->edx, STATFS_MAGIC_SYSFS);
    }
    if (e->type == FD_TYPE_SYNTH_PATH) {
        const struct vpath *vp = (const struct vpath *)e->priv;
        int dev = vp && (vp->kind == VNODE_DEV || vp->kind == VNODE_PTS);
        uint32_t magic = dev ? STATFS_MAGIC_DEVFS : STATFS_MAGIC_PROC;

        if (synth_is_sys(vp)) {
            magic = STATFS_MAGIC_SYSFS;
        }
        return statfs_fill((struct linux_statfs64 __user *)params->edx, magic);
    }
    if (e->type == FD_TYPE_DEVICE || e->type == FD_TYPE_CONSOLE || e->type == FD_TYPE_PTM ||
        e->type == FD_TYPE_SYNTH_DIR) {
        return statfs_fill((struct linux_statfs64 __user *)params->edx, STATFS_MAGIC_DEVFS);
    }
    if (e->type != FD_TYPE_FILE && e->type != FD_TYPE_DIR) {
        return -ENOSYS;
    }
    if (e->ofd && e->ofd->vmnt && !mnt_is_root(e->ofd->vmnt)) {
        return statfs_fill_mount((struct linux_statfs64 __user *)params->edx, e->ofd->vmnt);
    }
    return statfs_fill((struct linux_statfs64 __user *)params->edx, STATFS_MAGIC_KRFFS);
}

/**
 * Fill a stat64 from an open descriptor by type.
 *
 * Shared by sys_fstat64 and the followed /proc/<pid>/fd/<n> path (whose
 * target is just this fd). Where the object has an identity of its own (a
 * socket, a pipe, a device, a /proc snapshot, a KRFFS node), the inode
 * reported here is that identity and is the same number the descriptor's
 * `/proc/<pid>/fd` link carries, so a program reading one can match the other.
 */
static int32_t fstat_fill(fd_entry_t *entry, struct linux_stat64 *kst)
{
    k_memset(kst, 0, sizeof(*kst));
    stat64_boot_times(kst); /* a node with times of its own overwrites these */
    if ((entry->type == FD_TYPE_FILE || entry->type == FD_TYPE_DIR) && entry->ofd &&
        entry->ofd->vmnt && !mnt_is_root(entry->ofd->vmnt)) {
        struct vfs_ref r;

        r.mnt = entry->ofd->vmnt;
        r.node = entry->ofd->vnode;
        return vfs_fill_stat64(kst, &r);
    }
    if (entry->type == FD_TYPE_DIR && entry->node == NULL) {
        kst->st_mode = S_IFDIR | 0755; /* the root directory */
        kst->st_ino = kst->__st_ino = 1;
        kst->st_nlink = 1;
        kst->st_blksize = 4096;
        /* The root has times of its own, so a descriptor on it reports what a
         * path to it reports; the boot-time default above is for the nodes that
         * have no history, and the root is not one of them. */
        stat64_root_times(kst);
    } else if (entry->node) {
        /* A stale fd (its node was unlinked or relocated) would leak freed
         * free-list bytes through the stat fields; leave the zeroed stat rather
         * than read a node that is no longer live. */
        if (fd_node_check(entry) == 0) {
            krffs_fill_stat64(kst, entry->node);
        }
    } else if (entry->type == FD_TYPE_SOCKET) {
        /* A socket fd: programs classify these with S_ISSOCK, and the inode is
         * the socket's own, the same number /proc/net/tcp and the fd symlink
         * report, so a program can match a row to a descriptor. */
        kst->st_mode = 0140666; /* S_IFSOCK | 0666 */
        kst->st_nlink = 1;
        kst->st_blksize = 4096;
        kst->st_ino = entry->sock ? entry->sock->inode : 0u;
    } else if (entry->type == FD_TYPE_UNIX) {
        /* An AF_UNIX socket: S_ISSOCK, and the inode /proc/PID/fd shows. */
        kst->st_mode = 0140777; /* S_IFSOCK | 0777 */
        kst->st_nlink = 1;
        kst->st_blksize = 4096;
        kst->st_ino = entry->priv ? unix_inode(entry) : 0u;
    } else if (entry->type == FD_TYPE_ANON) {
        /* eventfd, epoll, timerfd: Linux's anonymous inode, mode 0600 with
         * no type bits. */
        kst->st_mode = 0600;
        kst->st_nlink = 1;
        kst->st_blksize = 4096;
    } else if (entry->type == FD_TYPE_PIPE) {
        /* A pipe end: S_ISFIFO, and the pipe's own inode (see fd.h). */
        pipe_t *pp = (pipe_t *)entry->priv;
        kst->st_mode = 010666; /* S_IFIFO | 0666 */
        kst->st_nlink = 1;
        kst->st_blksize = 4096;
        kst->st_ino = pp ? pp->inode : 0u;
    } else if (entry->type == FD_TYPE_DEVICE) {
        /* A devfs character device: report its identity so fstat matches the
         * stat of its /dev path (what ttyname compares), the mode included. */
        kst->st_mode = devfs_fd_mode(entry);
        kst->st_nlink = 1;
        kst->st_blksize = 4096;
        kst->st_dev = DEVFS_FSID;
        kst->st_rdev = devfs_rdev(entry);
        kst->st_ino = devfs_fd_ino(entry);
    } else if (entry->type == FD_TYPE_PROC) {
        /* A /proc text snapshot: a regular file whose inode is its origin's,
         * so fstat matches the stat of the path it was opened by, and whose
         * mode and owner are too: a process's file is its owner's, as
         * `synth_pid_owner` has it, and mode 0644 for oom_score_adj. */
        uint32_t uid, gid;
        int owned;

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
        if (owned == 1 || procfs_fd_writable_sysctl(entry)) {
            kst->st_mode = 0100644; /* S_IFREG | 0644 */
        }
    } else if (entry->type == FD_TYPE_CONSOLE) {
        /* stdin/stdout/stderr and /dev/console: report the same identity as
         * stat("/dev/console") so ttyname's (st_dev, st_ino) comparison
         * matches. */
        uint32_t cmode = 0, crdev = 0, cino = 0;
        (void)devfs_stat("console", &cmode, &crdev, &cino);
        kst->st_mode = 020666; /* S_IFCHR | 0666 */
        kst->st_nlink = 1;
        kst->st_blksize = 4096;
        kst->st_dev = DEVFS_FSID;
        kst->st_rdev = crdev;
        kst->st_ino = cino;
    } else if (entry->type == FD_TYPE_SYNTH_DIR) {
        /* A /dev or /proc directory fd: report the SAME identity its path stat
         * would (so fstat and stat agree on the directory), reconstructed from
         * the fd's record of its kind, pid and tid. */
        struct vpath vp;

        synth_dir_vpath((const struct synth_dir *)entry->priv, &vp);
        if (synth_stat(&vp, kst, 1) != 0) {
            kst->st_mode = S_IFDIR | 0555; /* the process exited: a bare directory */
            kst->st_nlink = 1;
            kst->st_blksize = 4096;
            kst->st_dev = PROCFS_FSID;
            if (synth_is_sys(&vp)) {
                synth_sys_identity(&vp, kst, 1);
            }
        }
    } else if (entry->type == FD_TYPE_SYNTH_PATH) {
        /* An O_PATH descriptor on a /dev or /proc leaf, or on a /proc link
         * under O_NOFOLLOW: what an lstat of its path reports while the node
         * is there, and a bare device or file once it is gone. */
        const struct vpath *vp = (const struct vpath *)entry->priv;

        if (!vp || synth_stat(vp, kst, 0) != 0) {
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
    } else if (entry->type == FD_TYPE_PTS) {
        /* A pty slave: report the SAME identity as stat("/dev/pts/N") so
         * ttyname's (st_dev, st_ino) comparison matches the /proc/self/fd
         * readlink. */
        struct vpath vp;
        k_memset(&vp, 0, sizeof(vp));
        vp.kind = VNODE_PTS;
        vp.pts_index = pty_slave_index(entry);
        if (synth_stat(&vp, kst, 1) != 0) {
            kst->st_mode = 020620; /* S_IFCHR | 0620 (master hung up) */
            kst->st_nlink = 1;
            kst->st_blksize = 4096;
            kst->st_dev = DEVFS_FSID;
        }
    } else {
        /* A pty master, a framebuffer or a mouse fd: a character device,
         * size 0. */
        kst->st_mode = 020666; /* S_IFCHR | 0666 */
        kst->st_nlink = 1;
        kst->st_blksize = 4096;
    }
    return 0;
}

/*
 * fstat64 (syscall 197): EBX = fd, ECX = stat64 buffer. The descriptor's
 * node is stat'd; a root directory fd (node == NULL) reports the root
 * directory.
 */
int32_t sys_fstat64(isr_params_t *params)
{
    int32_t rc;
    int fd = (int)params->ebx;
    struct linux_stat64 __user *st = (struct linux_stat64 __user *)params->ecx;
    struct linux_stat64 kst;
    fd_entry_t *entry;

    if (!current_proc || !st) {
        return -EFAULT;
    }
    entry = fd_get(current_proc->fds, fd);
    if (!entry) {
        return -EBADF;
    }
    rc = fstat_fill(entry, &kst);
    if (rc != 0) {
        return rc;
    }
    if (copy_to_user(st, &kst, sizeof(kst)) != 0) {
        return -EFAULT;
    }
    return 0;
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
static int32_t fstatat_path(const char *path, int follow, struct linux_stat64 *kst)
{
    struct vfs_ref ref;
    const struct vpath *svp;
    int32_t rc;

    k_memset(kst, 0, sizeof(*kst));
    if (names_tree_root(path) && vfs_root_mount()->type == &vfs_krffs_type) {
        kst->st_mode = S_IFDIR | 0755;
        kst->st_ino = kst->__st_ino = 1;
        kst->st_nlink = 1;
        kst->st_blksize = 4096;
        stat64_root_times(kst);
        return 0;
    }
    rc = vfs_resolve(0, path, VFS_WALK_LOCK | (follow ? 0 : VFS_WALK_NOFOLLOW), &ref, 0, 0);
    if (rc != 0) {
        return rc;
    }
    svp = vfs_synth_vpath(ref.mnt, ref.node);
    if (svp) {
        rc = synth_stat(svp, kst, follow);
    } else if (!mnt_is_root(ref.mnt)) {
        rc = vfs_fill_stat64(kst, &ref);
    } else {
        krffs_fill_stat64(kst, (krffs_node_t *)ref.node);
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
static int32_t stat_at(int dirfd, const char __user *upath, int flags, struct linux_stat64 *kst)
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

        return entry ? fstat_fill(entry, kst) : -EBADF;
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
 * fstatat64 (syscall 300): EBX = dirfd, ECX = path, EDX = stat64 buffer, ESI
 * = flags.
 *
 * The attributes are `stat_at`'s; GNU libc's fstat is this call with
 * AT_EMPTY_PATH when statx is missing. AT_NO_AUTOMOUNT is accepted and
 * means nothing here, and any other flag is -EINVAL before the path is read.
 */
int32_t sys_fstatat64(isr_params_t *params)
{
    struct linux_stat64 __user *st = (struct linux_stat64 __user *)params->edx;
    int flags = (int)params->esi;
    struct linux_stat64 kst;
    int32_t rc;

    if (!current_proc || !st) {
        return -EFAULT;
    }
    if (flags & ~(AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH | AT_NO_AUTOMOUNT)) {
        return -EINVAL;
    }
    rc = stat_at((int)params->ebx, (const char __user *)params->ecx, flags, &kst);
    if (rc == 0 && copy_to_user(st, &kst, sizeof(kst)) != 0) {
        rc = -EFAULT;
    }
    return rc;
}

/* The statx flags that ask how fresh an answer must be; this kernel has one
 * answer, and both at once are EINVAL. */
#define AT_STATX_SYNC_TYPE 0x6000

/* The attributes statx reports, and the mask bit reserved for a larger
 * structure, which is EINVAL. */
#define STATX_BASIC_STATS 0x7FFu
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
    uint64_t spare[14];
};

typedef char linux_statx_is_256_bytes[sizeof(struct linux_statx) == 256 ? 1 : -1];

/* The major and minor numbers of a device number in Linux's 32-bit
 * encoding, which st_dev and st_rdev have. */
static uint32_t dev_major(uint64_t dev)
{
    return (uint32_t)(dev >> 8) & 0xFFFu;
}

static uint32_t dev_minor(uint64_t dev)
{
    return ((uint32_t)dev & 0xFFu) | (((uint32_t)dev >> 12) & 0xFFF00u);
}

/**
 * Put the attributes of `st` into `sx` as statx reports them: the basic set,
 * the device numbers split into their major and minor numbers, and the times
 * as signed 64-bit seconds, which is how musl reads the 32-bit ones.
 */
static void statx_from_stat64(struct linux_statx *sx, const struct linux_stat64 *st)
{
    k_memset(sx, 0, sizeof(*sx));
    sx->stx_mask = STATX_BASIC_STATS;
    sx->stx_blksize = (uint32_t)st->st_blksize;
    sx->stx_nlink = st->st_nlink;
    sx->stx_uid = (uint32_t)st->st_uid;
    sx->stx_gid = (uint32_t)st->st_gid;
    sx->stx_mode = (uint16_t)st->st_mode;
    sx->stx_ino = st->st_ino;
    sx->stx_size = (uint64_t)st->st_size;
    sx->stx_blocks = st->st_blocks;
    sx->stx_atime.tv_sec = (int32_t)st->st_atime;
    sx->stx_atime.tv_nsec = (uint32_t)st->st_atime_nsec;
    sx->stx_ctime.tv_sec = (int32_t)st->st_ctime;
    sx->stx_ctime.tv_nsec = (uint32_t)st->st_ctime_nsec;
    sx->stx_mtime.tv_sec = (int32_t)st->st_mtime;
    sx->stx_mtime.tv_nsec = (uint32_t)st->st_mtime_nsec;
    sx->stx_rdev_major = dev_major(st->st_rdev);
    sx->stx_rdev_minor = dev_minor(st->st_rdev);
    sx->stx_dev_major = dev_major(st->st_dev);
    sx->stx_dev_minor = dev_minor(st->st_dev);
}

/**
 * statx (syscall 383): EBX = dirfd, ECX = path, EDX = flags, ESI = mask,
 * EDI = struct statx buffer.
 *
 * The attributes are fstatat64's, found by `stat_at` for the same path and
 * flags, in struct statx: the basic set of STATX_BASIC_STATS whatever `mask`
 * asks for, since the kernel may report more than it is asked, with no birth
 * time, no attributes and no mount id. The two sync flags are accepted and
 * change nothing; both at once, a flag fstatat64 does not take, or
 * STATX__RESERVED in `mask` is EINVAL. GNU libc and musl call this first for
 * every stat and fall back to the stat64 calls on ENOSYS alone.
 */
int32_t sys_statx(isr_params_t *params)
{
    int flags = (int)params->edx;
    void __user *out = (void __user *)params->edi;
    struct linux_stat64 kst;
    struct linux_statx sx;
    int32_t rc;

    if (!current_proc) {
        return -EFAULT;
    }
    if ((flags & ~(AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH | AT_NO_AUTOMOUNT | AT_STATX_SYNC_TYPE)) ||
        (flags & AT_STATX_SYNC_TYPE) == AT_STATX_SYNC_TYPE || (params->esi & STATX_RESERVED)) {
        return -EINVAL;
    }
    rc = stat_at((int)params->ebx, (const char __user *)params->ecx, flags, &kst);
    if (rc != 0) {
        return rc;
    }
    statx_from_stat64(&sx, &kst);
    return copy_to_user(out, &sx, sizeof(sx)) != 0 ? -EFAULT : 0;
}

/* access(2) mode bits. */
#define F_OK 0

#define X_OK 1

#define W_OK 2

#define R_OK 4

/**
 * access core: does the path exist and grant the requested mode?
 *
 * R/W/X are checked against the object's mode bits by the caller's REAL ids
 * (the credential model access(2) uniquely uses), which is what makes
 * /proc/<pid>/stat reject W_OK while granting F_OK/R_OK. `name` is absolute
 * with the leading '/' stripped, and a path that named a directory by its
 * end, `dir`, must reach one (`resolve_named`). The walk keeps the lock of
 * the mount it ends on until the attributes are read, as `do_stat64`'s does,
 * and for the same reason.
 */
static int32_t do_access(const char *name, int mode, int dir)
{
    struct vfs_ref ref;
    const struct vpath *vp;
    int32_t rc;
    int werr;

    if (mode & ~(R_OK | W_OK | X_OK)) {
        return -EINVAL; /* undefined access-mode bits */
    }
    /* access(2) is the one caller judged by the REAL ids: it exists so a
     * set-id program can ask what its invoker could do. The walk charges
     * search the same way, so a set-id program cannot report reachability
     * its caller does not have. */
    werr = resolve_named(name, VFS_WALK_REAL_IDS | VFS_WALK_LOCK, dir, &ref);
    if (werr != 0) {
        return werr;
    }
    vp = vfs_synth_vpath(ref.mnt, ref.node);
    if (vp) {
        struct linux_stat64 s;

        rc = synth_stat(vp, &s, 1); /* access follows symlinks */
        vfs_walk_unlock(ref.mnt);
        vfs_ref_release(&ref);
        if (rc != 0) {
            return rc; /* -ENOENT / -ENOTDIR */
        }
        /* Synthetic metadata is fixed and root-owned; judge it by the real
         * ids like any other access(2) answer. */
        return cred_access_bits(s.st_uid, s.st_gid, s.st_mode & 0777u, mode, 1);
    }
    /* R_OK/W_OK/X_OK are the same octal digits the checker takes. */
    if (!mnt_is_root(ref.mnt)) {
        rc = cred_access_ref(&ref, mode, 1);
        if (rc == 0 && (mode & W_OK) && (ref.mnt->flags & VFS_MS_RDONLY)) {
            rc = -EROFS; /* access(2) reports the mount's refusal too */
        }
    } else {
        rc = cred_access_node((krffs_node_t *)ref.node, mode, 1);
    }
    vfs_walk_unlock(ref.mnt);
    vfs_ref_release(&ref);
    return rc;
}

int32_t sys_access(isr_params_t *params)
{
    const char __user *upath = (const char __user *)params->ebx;
    int mode = (int)params->ecx;
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
    rc = do_access(path_name(path), mode, dir);
    path_buf_put(path);
    return rc;
}

/**
 * inotify_add_watch(292): EBX the instance, ECX the path, EDX the mask.
 *
 * The path is resolved through a final link unless IN_DONT_FOLLOW, it must
 * name a directory under IN_ONLYDIR, and the caller must be able to read
 * what it names, as on Linux; `inotify_watch` adds the watch. A node of
 * /dev or /proc can be watched and reports nothing, since no call that
 * changes one tells inotify.
 */
int32_t sys_inotify_add_watch(isr_params_t *params)
{
    int fd = (int)params->ebx;
    const char __user *upath = (const char __user *)params->ecx;
    uint32_t mask = params->edx;
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
    if (vp) {
        struct linux_stat64 s;

        rc = synth_stat(vp, &s, !(mask & IN_DONT_FOLLOW));
        if (rc == 0) {
            rc = cred_access_bits(s.st_uid, s.st_gid, s.st_mode & 0777u, CRED_R, 0);
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

int32_t sys_faccessat(isr_params_t *params)
{
    int dirfd = (int)params->ebx;
    const char __user *upath = (const char __user *)params->ecx;
    int mode = (int)params->edx;
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
    rc = do_access(path_name(path), mode, dir);
    path_buf_put(path);
    return rc;
}

/**
 * The chmod rule over a resolved KRFFS node: the owner or root replaces the
 * permission bits; anyone else is refused. The root node's mode is never
 * touched (its spare bits carry the dirty-mount flag), but the rule still
 * answers, so a non-owner probing `/` gets the correct -EPERM.
 */
static int32_t krffs_chmod_node(krffs_node_t *node, uint32_t mode)
{
    if (!cred_privileged() && current_proc->euid != node->uid) {
        return -EPERM;
    }
    if (node != krffs_fs->node) {
        uint32_t want = mode & 07777u;

        /* chmod(2)'s own rule: an unprivileged caller storing a mode on a file
         * whose group is not one of theirs does not get to keep setgid. This is
         * the third place POSIX names a set-id clear, after write and chown,
         * and it is the one no write-clear can stand in for: an owner may mint
         * 02710 on a gid-0 file they own by chmod alone, with no write ever
         * running. Deliberately NOT conditioned on group-execute the way
         * cred_kill_setid is, because the caller is choosing that bit in this
         * same call, so reading it back as a reason to spare the mode would
         * defeat the rule. Setuid needs no rule here: only the owner may chmod,
         * and the setuid grant is that owner's own identity. */
        if (!cred_privileged() && !KRFFS_IS_DIR(node->mode) && !cred_in_group(node->gid)) {
            want &= ~(uint32_t)KRFFS_MODE_SGID;
        }
        node->mode = (uint16_t)((node->mode & ~07777u) | want);
        krffs_stamp(node, KRFFS_STAMP_ATTR);
    }
    return 0;
}

/**
 * The chown rule over a resolved KRFFS node, the POSIX split: changing the
 * owner takes root; changing the group takes root, or being the owner and
 * naming one of your own groups. `(id_t)-1` keeps a field, and a no-change
 * value needs no right at all, so `chown(path, -1, -1)` always succeeds on an
 * existing file.
 */
static int32_t krffs_chown_node(krffs_node_t *node, uint32_t uid, uint32_t gid)
{
    int uid_changes = (uid != (uint32_t)-1 && uid != node->uid);
    int gid_changes = (gid != (uint32_t)-1 && gid != node->gid);

    if (uid_changes && !cred_privileged()) {
        return -EPERM;
    }
    if (gid_changes && !cred_privileged() &&
        (current_proc->euid != node->uid || !cred_in_group(gid))) {
        return -EPERM;
    }
    if (uid != (uint32_t)-1) {
        node->uid = uid;
    }
    if (gid != (uint32_t)-1) {
        node->gid = gid;
    }
    /* Stamped once, after both fields, and on the same gate the set-id clear
     * below uses: a chown that changed an owner changed the node's attributes,
     * whichever of the two it was, and chown(path, -1, -1) changed nothing and
     * must leave the times alone. Stamping inside the uid branch, which is
     * where this started, left `chown(path, -1, gid)` unstamped. */
    /* The set-id clear is not gated on privilege: POSIX drops the bits whoever
     * asks, which is why cp and install chown before they chmod. Both are gated
     * on a field ACTUALLY having changed, and that gate has real weight:
     * chown(path, -1, -1) succeeds for every caller by design, since a value
     * that changes nothing needs no right, so a clear that ran on it would let
     * any uid disarm /bin/su with one call. */
    if (uid_changes || gid_changes) {
        krffs_stamp(node, KRFFS_STAMP_ATTR);
        cred_kill_setid(node);
    }
    return 0;
}

/**
 * chmod core for the chmod/chown family.
 *
 * A KRFFS path stores through `krffs_chmod_node` under the owner-or-root rule.
 * A pty slave (`/dev/pts/N`) stores on the pty, the state dropbear moves
 * before handing the terminal to the user. Every other synthetic /dev or
 * /proc object has fixed root-owned metadata: root's request succeeds
 * without a change, anyone else's is refused. `name` is absolute with the
 * leading '/' stripped, and must reach a directory when it named one by its
 * end, `dir`, matching do_access.
 *
 * The walk takes the lock of the mount it ends on, where the type asks for
 * one, and the lookup, the check and the store are one hold of it: a node of
 * a mounted KRFFS volume is its offset, which a growing write that held the
 * lock would move, and a store made at the offset the walk found would
 * change whatever the volume put there since.
 */
static int32_t do_chmod(const char *name, uint32_t mode, int dir)
{
    struct vpath vp;
    const struct vpath *svp;
    struct vfs_ref ref;
    krffs_node_t *node;
    int32_t rc;

    rc = resolve_named(name, VFS_WALK_LOCK, dir, &ref);
    if (rc != 0) {
        return rc;
    }
    svp = vfs_synth_vpath(ref.mnt, ref.node);
    if (svp) {
        vp = *svp;
        vfs_walk_unlock(ref.mnt);
        vfs_ref_release(&ref);
        if (vp.kind == VNODE_PTS) {
            struct pty *pt = pty_get(vp.pts_index);
            if (!pt) {
                return -ENOENT;
            }
            if (!cred_privileged() && current_proc->euid != pt->uid) {
                return -EPERM;
            }
            pt->mode = (uint16_t)(mode & 0777u);
            return 0;
        }
        return current_proc->euid == 0 ? 0 : -EPERM; /* fixed metadata root owns */
    }
    if (!mnt_is_root(ref.mnt)) {
        struct vfs_stat vst;

        vfs_ns_lock(ref.mnt);
        rc = vfs_getattr(ref.mnt, ref.node, &vst);
        if (rc == 0 && !cred_privileged() && current_proc->euid != vst.uid) {
            rc = -EPERM;
        }
        if (rc == 0) {
            if (!cred_privileged() && !cred_in_group(vst.gid)) {
                mode &= ~02000u; /* set-gid on a group not one's own is dropped */
            }
            vst.mode = mode;
            rc = vfs_setattr(ref.mnt, ref.node, VFS_SET_MODE, &vst);
        }
        vfs_ns_unlock(ref.mnt);
        vfs_walk_unlock(ref.mnt);
        vfs_ref_release(&ref);
        return rc;
    }
    node = ref.node;
    rc = krffs_chmod_node(node, mode);
    vfs_walk_unlock(ref.mnt);
    vfs_ref_release(&ref);
    return rc;
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
    if (!cred_privileged() && (current_proc->euid != vst->uid || (what & VFS_SET_UID) != 0 ||
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

/**
 * chown of the /dev or /proc node `vp`, for the path forms and for a
 * descriptor opened with O_PATH on one. A pty slave keeps its owner and group
 * on the pty, and changing either takes root, since the login server that
 * chowns a pty is root. Every other node has fixed root ownership: a request
 * for 0 or -1 in each field succeeds and changes nothing, and any other is
 * -EPERM.
 */
static int32_t chown_synth(const struct vpath *vp, uint32_t uid, uint32_t gid)
{
    if (vp->kind == VNODE_PTS) {
        struct pty *pt = pty_get(vp->pts_index);
        int uid_changes;
        int gid_changes;

        if (!pt) {
            return -ENOENT;
        }
        uid_changes = (uid != (uint32_t)-1 && uid != pt->uid);
        gid_changes = (gid != (uint32_t)-1 && gid != pt->gid);
        if ((uid_changes || gid_changes) && !cred_privileged()) {
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

        /* The read, the plan and the store are one step under the mount's
         * namespace lock. Taking the lock inside `vfs_setattr` would leave the
         * plan made from attributes another process can change while this one
         * waits for that lock: a privileged chown planned over a 0755 file
         * would store the new owner without the set-id clear the file earned
         * from a chmod that landed in between, which is a set-id program with
         * an owner the caller chose. The lock is recursive for its holder, so
         * the calls below take it again for nothing. */
        vfs_ns_lock(ref.mnt);
        rc = vfs_getattr(ref.mnt, ref.node, &vst);
        if (rc == 0) {
            what = vfs_chown_plan(&vst, uid, gid);
            rc = what < 0 ? what : (what ? vfs_setattr(ref.mnt, ref.node, what, &vst) : 0);
        }
        vfs_ns_unlock(ref.mnt);
        vfs_walk_unlock(ref.mnt);
        vfs_ref_release(&ref);
        return rc;
    }
    node = ref.node;
    rc = krffs_chown_node(node, uid, gid);
    vfs_walk_unlock(ref.mnt);
    vfs_ref_release(&ref);
    return rc;
}

/* chmod(path, mode) and fchmodat(dirfd, path, mode[, flags]) share do_chmod. */
int32_t sys_chmod(isr_params_t *params)
{
    const char __user *upath = (const char __user *)params->ebx;
    uint32_t mode = params->ecx;
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
    rc = do_chmod(path_name(path), mode, dir);
    path_buf_put(path);
    return rc;
}

int32_t sys_fchmodat(isr_params_t *params)
{
    int dirfd = (int)params->ebx;
    const char __user *upath = (const char __user *)params->ecx;
    uint32_t mode = params->edx;
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
    rc = do_chmod(path_name(path), mode, dir);
    path_buf_put(path);
    return rc;
}

/* fchmod(fd, mode): the descriptor form, under the rules do_chmod keeps:
 * a KRFFS node or a node of another mounted file system stores the mode, a
 * pty slave stores it on the pty, and every other descriptor has fixed
 * metadata (root succeeds without a change, anyone else is refused). An
 * O_PATH descriptor is -EBADF, as on Linux. */
int32_t sys_fchmod(isr_params_t *params)
{
    int fd = (int)params->ebx;
    uint32_t mode = params->ecx;
    fd_entry_t *entry;

    if (!current_proc) {
        return -ESRCH;
    }
    entry = fd_get(current_proc->fds, fd);
    if (!entry || (entry->flags & FD_FLAG_PATH)) {
        return -EBADF;
    }
    if ((entry->type == FD_TYPE_FILE || entry->type == FD_TYPE_DIR) && entry->node) {
        return krffs_chmod_node(entry->node, mode);
    }
    if ((entry->type == FD_TYPE_FILE || entry->type == FD_TYPE_DIR) && entry->ofd &&
        entry->ofd->vmnt && entry->ofd->vnode) {
        /* A node of another type, by the rules chmod(2) keeps for one. */
        struct vfs_stat vst;
        int rc;

        vfs_ns_lock(entry->ofd->vmnt);
        rc = vfs_getattr(entry->ofd->vmnt, entry->ofd->vnode, &vst);
        if (rc == 0 && !cred_privileged() && current_proc->euid != vst.uid) {
            rc = -EPERM;
        }
        if (rc == 0) {
            if (!cred_privileged() && !cred_in_group(vst.gid)) {
                mode &= ~02000u;
            }
            vst.mode = mode;
            rc = vfs_setattr(entry->ofd->vmnt, entry->ofd->vnode, VFS_SET_MODE, &vst);
        }
        vfs_ns_unlock(entry->ofd->vmnt);
        return rc;
    }
    if (entry->type == FD_TYPE_PTS) {
        struct pty *pt = pty_get(pty_slave_index(entry));
        if (!pt) {
            return -EBADF;
        }
        if (!cred_privileged() && current_proc->euid != pt->uid) {
            return -EPERM;
        }
        pt->mode = (uint16_t)(mode & 0777u);
        return 0;
    }
    /* Every other descriptor has fixed metadata root owns, like its path. */
    return current_proc->euid == 0 ? 0 : -EPERM;
}

/** The path form of chown and lchown: EBX = path, ECX = uid, EDX = gid. */
static int32_t chown_path(const char __user *upath, uint32_t uid, uint32_t gid, int follow)
{
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
    rc = do_chown(path_name(path), uid, gid, follow, dir);
    path_buf_put(path);
    return rc;
}

/**
 * chown(2), 182 and 212: through a final symbolic link. The 16-bit number
 * takes 32-bit ids here, as its *32 twin does.
 */
int32_t sys_chown(isr_params_t *params)
{
    return chown_path((const char __user *)params->ebx, params->ecx, params->edx, 1);
}

/** lchown(2), 16 and 198: a final symbolic link itself, as on Linux. */
int32_t sys_lchown(isr_params_t *params)
{
    return chown_path((const char __user *)params->ebx, params->ecx, params->edx, 0);
}

static int32_t chown_fd(fd_entry_t *entry, uint32_t uid, uint32_t gid);

/**
 * fchownat(2): EBX = dirfd, ECX = path, EDX = uid, ESI = gid, EDI = flags.
 *
 * The flags are Linux's two, checked first: AT_SYMLINK_NOFOLLOW changes a
 * final symbolic link itself, and AT_EMPTY_PATH with an empty path changes
 * the file `dirfd` is open on, an O_PATH descriptor's included, or the
 * working directory for AT_FDCWD; any other bit is -EINVAL. An empty path
 * without AT_EMPTY_PATH is -ENOENT, and a NULL one -EFAULT.
 */
int32_t sys_fchownat(isr_params_t *params)
{
    int dirfd = (int)params->ebx;
    const char __user *upath = (const char __user *)params->ecx;
    uint32_t uid = params->edx;
    uint32_t gid = params->esi;
    int flags = (int)params->edi;
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

/* fchown(fd, uid, gid): the descriptor form, under the same ownership rule
 * as the path forms, on the node or the mounted volume the descriptor is
 * open on. An O_PATH descriptor is -EBADF, as on Linux. */
int32_t sys_fchown(isr_params_t *params)
{
    int fd = (int)params->ebx;
    fd_entry_t *entry;

    if (!current_proc) {
        return -ESRCH;
    }
    entry = fd_get(current_proc->fds, fd);
    if (!entry || (entry->flags & FD_FLAG_PATH)) {
        return -EBADF;
    }
    return chown_fd(entry, params->ecx, params->edx);
}

/**
 * chown of the file descriptor `entry` refers to, for fchown(2) and for
 * fchownat(2) with AT_EMPTY_PATH, which takes an O_PATH descriptor too: a
 * KRFFS node or a node of a mounted type by the rules of the path forms, a
 * /dev or /proc node opened with O_PATH by `chown_synth`, a pty slave on the
 * pty, and every other kind with fixed root ownership.
 */
static int32_t chown_fd(fd_entry_t *entry, uint32_t uid, uint32_t gid)
{
    if (entry->type == FD_TYPE_SYNTH_PATH) {
        return entry->priv ? chown_synth((const struct vpath *)entry->priv, uid, gid) : -EBADF;
    }
    if ((entry->type == FD_TYPE_FILE || entry->type == FD_TYPE_DIR) && entry->node) {
        return krffs_chown_node(entry->node, uid, gid);
    }
    if ((entry->type == FD_TYPE_FILE || entry->type == FD_TYPE_DIR) && entry->ofd &&
        entry->ofd->vmnt && entry->ofd->vnode) {
        /* A node of another type, by the rules chown(2) keeps for one. */
        struct vfs_stat vst;
        int what;
        int rc;

        /* The read, the plan and the store are one step under the mount's
         * namespace lock. Taking the lock inside `vfs_setattr` would leave the
         * plan made from attributes another process can change while this one
         * waits for that lock: a privileged chown planned over a 0755 file
         * would store the new owner without the set-id clear the file earned
         * from a chmod that landed in between, which is a set-id program with
         * an owner the caller chose. The lock is recursive for its holder, so
         * the calls below take it again for nothing. */
        vfs_ns_lock(entry->ofd->vmnt);
        rc = vfs_getattr(entry->ofd->vmnt, entry->ofd->vnode, &vst);
        if (rc == 0) {
            what = vfs_chown_plan(&vst, uid, gid);
            rc = what < 0
                     ? what
                     : (what ? vfs_setattr(entry->ofd->vmnt, entry->ofd->vnode, what, &vst) : 0);
        }
        vfs_ns_unlock(entry->ofd->vmnt);
        return rc;
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
        if ((uid_changes || gid_changes) && !cred_privileged()) {
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
    /* Every other descriptor has fixed root-owned metadata, like its path:
     * a no-change request succeeds, a change is refused. */
    if ((uid != (uint32_t)-1 && uid != 0) || (gid != (uint32_t)-1 && gid != 0)) {
        return -EPERM;
    }
    return 0;
}

/**
 * The refusals of truncate(2) for the file `ref` resolved to, in Linux's
 * order: a directory is -EISDIR and anything but a regular file -EINVAL, a
 * read-only mount -EROFS, a caller without write permission -EACCES, and a
 * file a loop device holds -EBUSY. A /proc file is -EACCES to every caller,
 * root included, as an open of one for writing is. Returns 0 or the errno.
 */
static int32_t truncate_checks(const struct vfs_ref *ref)
{
    const struct vpath *vp = vfs_synth_vpath(ref->mnt, ref->node);
    krffs_node_t *node = ref->node;
    uint32_t mode;
    int32_t rc;

    if (vp) {
        struct linux_stat64 kst;

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
 * `length` bytes, which is not negative, for truncate(2) and truncate64(2).
 *
 * The path is resolved through links, to a directory when it named one by
 * its end, `dir` (`resolve_named`), and `truncate_checks` has what is
 * refused. The file is then truncated through an open-file description of
 * its own, which no descriptor points at, with the open, the lock and the
 * close of an open for writing and an ftruncate: a KRFFS file that must move
 * to grow repoints the description, so the set-id clear of an unprivileged
 * caller finds the file where it is now, and a loop attachment sees the
 * truncation in progress, as it sees an ftruncate. Linux has ETXTBSY where
 * a program being run is truncated, and no loop rule; this kernel has the
 * loop rule and no ETXTBSY.
 */
static int32_t truncate_path(const char *path, long long length, int dir)
{
    struct vfs_ref ref;
    struct vfs_mount *locked;
    struct open_file *desc = NULL;
    int32_t rc;

    rc = resolve_named(path_name(path), VFS_WALK_LOCK, dir, &ref);
    if (rc != 0) {
        return rc;
    }
    locked = ref.mnt;
    rc = truncate_checks(&ref);
    if (rc == 0) {
        desc = fd_ofd_new();
        rc = desc ? vfs_open_node(ref.mnt, ref.node, 1 /* O_WRONLY */, &desc->fhandle) : -ENOMEM;
    }
    if (rc != 0) {
        fd_ofd_unref(desc);
        vfs_walk_unlock(locked);
        vfs_ref_release(&ref);
        return rc;
    }
    desc->vmnt = ref.mnt;
    desc->vnode = ref.node;
    vfs_desc_open(ref.mnt);
    vfs_leave(ref.mnt);
    fd_ofd_lock(desc);
    rc = -EBUSY;
    if (!loop_backs(desc->vmnt, desc->vnode)) {
        rc = vfs_truncate(desc->vmnt, desc->fhandle, (uint64_t)length);
    }
    if (rc == 0 && !cred_privileged()) {
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
    fd_ofd_unlock(desc);
    fd_ofd_unref(desc);
    vfs_walk_unlock(locked);
    return rc;
}

/**
 * The shared body of truncate(2) and truncate64(2): a negative length is
 * -EINVAL before the path is looked at, as on Linux, and the path is copied
 * into a path page for `truncate_path`.
 */
static int32_t truncate_user(const char __user *upath, long long length)
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

/** truncate(2): EBX = path, ECX = the length, a signed 32-bit value. */
int32_t sys_truncate(isr_params_t *params)
{
    return truncate_user((const char __user *)params->ebx, (long long)(int32_t)params->ecx);
}

/**
 * truncate64(2): EBX = path, ECX and EDX = the length's low and high words.
 * Musl's and GNU libc's truncate are this call on i386.
 */
int32_t sys_truncate64(isr_params_t *params)
{
    uint64_t length = (uint64_t)params->ecx | ((uint64_t)params->edx << 32);

    return truncate_user((const char __user *)params->ebx, (long long)length);
}

/**
 * Walk to the parent of the new name `path`, with or without its leading
 * '/', for a call that makes a node other than a directory there:
 * `vfs_resolve` with VFS_WALK_PARENT, the last name in `leaf` of `leafcap`
 * bytes. The root, which exists and has no last name to make, is -EEXIST,
 * as Linux answers link(2), symlink(2) and mknod(2) there, and bind(2) as
 * EADDRINUSE.
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
 * Whether the name the path `path` ends in, in a directory of /dev or /proc,
 * exists: -EEXIST when it does, a final link not followed, and the walk's
 * error, -ENOENT for a name the namespace does not have, when it does not.
 * The two synthetic types look whole paths up, so the name is looked up by
 * `path`, and a device name they classify exists only when `synth_stat`
 * finds its device; any other failure of `synth_stat` is of a name that
 * exists.
 */
static int32_t synth_name_exists(const char *path)
{
    struct linux_stat64 kst;
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
 * them, and the lookup's -ENOENT for one that does not, in /dev and /proc
 * too (`synth_name_exists`), where the call's -EROFS comes after. 0 for any
 * other path.
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
 * /dev and /proc, of another mount, of a file system without the kind, and
 * of the write permission. A directory of /dev or /proc has the name looked
 * up by `path` (`synth_name_exists`).
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

/*
 * A hard link: the source resolved as itself unless `follow`, a directory
 * refused with EPERM, another mount with EXDEV, and the new name made by
 * the type under the namespace lock; a type without links answers EPERM,
 * and KRFFS, the root, is one. A source that named a directory by its end,
 * `olddir`, follows a final link and must reach a directory
 * (`resolve_named`), which is EPERM after it, and a new name that did,
 * `newend`, is `make_dir_end`'s; the root as the new name is EEXIST
 * (`new_name_parent`), and so is a new name that exists (`new_name_taken`),
 * before every refusal after the walks.
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
    /* Linux's order: the new name's end, a name that exists, another mount,
     * then a directory or a type without links, then a read-only mount, then
     * the parent's write bit. */
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
    } else if (vfs_getattr(src.mnt, src.node, &st) == 0 && (st.mode & 0170000u) == 0040000u) {
        rc = -EPERM;
    } else if (parent.mnt->flags & VFS_MS_RDONLY) {
        rc = -EROFS;
    } else {
        rc = (int32_t)cred_access_ref(&parent, CRED_W, 0);
    }
    if (rc == 0) {
        vfs_ns_lock(parent.mnt);
        rc = vfs_link(parent.mnt, parent.node, leaf, src.node);
        vfs_ns_unlock(parent.mnt);
    }
out:
    vfs_ref_release(&parent);
    vfs_ref_release(&src);
    return rc;
}

/*
 * A symbolic link `newname` to `target`, made by the type under the
 * namespace lock; a type without them answers EPERM. A new name that named a
 * directory by its end, `end`, is `make_dir_end`'s, and the root is EEXIST
 * (`new_name_parent`), and so is a new name that exists (`new_name_taken`),
 * before the refusals of a read-only mount, of /dev and /proc and of a type
 * without symbolic links.
 */
static int32_t do_symlink(const char *target, const char *newname, int end)
{
    struct vfs_ref parent;
    char leaf[VFS_NAME_MAX + 1];
    int32_t rc;

    if (target[0] == '\0') {
        return -ENOENT; /* an empty target, as Linux answers */
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
    if (vfs_synth_vpath(parent.mnt, parent.node) || (parent.mnt->flags & VFS_MS_RDONLY)) {
        rc = -EROFS;
    } else if (mnt_is_root(parent.mnt) || !parent.mnt->type->ops->symlink) {
        rc = -EPERM;
    } else {
        rc = (int32_t)cred_access_ref(&parent, CRED_W, 0);
    }
    if (rc == 0) {
        vfs_ns_lock(parent.mnt);
        rc = vfs_symlink(parent.mnt, parent.node, leaf, target);
        vfs_ns_unlock(parent.mnt);
    }
    vfs_ref_release(&parent);
    return rc;
}

/**
 * symlink(2) and symlinkat(2) after their arguments: the target is copied as
 * it is, up to `VFS_PATH_MAX` bytes with its terminator, and never resolved;
 * the new name is resolved from `newdirfd`.
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
        rc = -ENOENT; /* an empty target, as Linux answers */
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

int32_t sys_symlink(isr_params_t *params)
{
    const char __user *utarget = (const char __user *)params->ebx;
    const char __user *unew = (const char __user *)params->ecx;

    if (!current_proc || !utarget || !unew) {
        return -EFAULT;
    }
    return symlink_from_user(utarget, AT_FDCWD, unew);
}

int32_t sys_symlinkat(isr_params_t *params)
{
    const char __user *utarget = (const char __user *)params->ebx;
    int newdirfd = (int)params->ecx;
    const char __user *unew = (const char __user *)params->edx;

    if (!current_proc || !utarget || !unew) {
        return -EFAULT;
    }
    return symlink_from_user(utarget, newdirfd, unew);
}

/**
 * link(2) and linkat(2) after their arguments: both paths are copied, which
 * reports a bad pointer as -EFAULT and a bad directory descriptor as -EBADF,
 * before the source is looked up, so a missing source is -ENOENT and not the
 * -EPERM of a file system without links.
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

int32_t sys_link(isr_params_t *params)
{
    const char __user *uold = (const char __user *)params->ebx;
    const char __user *unew = (const char __user *)params->ecx;

    if (!current_proc || !uold || !unew) {
        return -EFAULT;
    }
    return link_from_user(AT_FDCWD, uold, AT_FDCWD, unew, 0);
}

int32_t sys_linkat(isr_params_t *params)
{
    int olddirfd = (int)params->ebx;
    const char __user *uold = (const char __user *)params->ecx;
    int newdirfd = (int)params->edx;
    const char __user *unew = (const char __user *)params->esi;
    int flags = (int)params->edi;

    if (!current_proc || !uold || !unew) {
        return -EFAULT;
    }
    /* Only AT_SYMLINK_FOLLOW and AT_EMPTY_PATH are defined for linkat. */
    if (flags & ~(AT_SYMLINK_FOLLOW | AT_EMPTY_PATH)) {
        return -EINVAL;
    }
    return link_from_user(olddirfd, uold, newdirfd, unew, (flags & AT_SYMLINK_FOLLOW) != 0);
}

/* The 64-bit displacement `_llseek` takes as two words. */
static long long off_from(uint32_t high, uint32_t low)
{
    return (long long)(((uint64_t)high << 32) | (uint64_t)low);
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
 * The current size of the file an ordinary descriptor is open on.
 *
 * A `SEEK_END` resolves against this, so the caller reads it inside the
 * description's lock: a process sharing the description can extend the
 * file while this one waits.
 *
 * @return 0, or -errno when the file is gone or its size cannot be read.
 */
static int file_size_now(fd_entry_t *entry, uint64_t *out)
{
    if (entry->node) {
        if (fd_node_check(entry) != 0) {
            return -EBADF; /* the file was unlinked or relocated out from under us */
        }
        *out = entry->node->data_size;
        return 0;
    }
    if (entry->ofd->vmnt) {
        /* A file of another type: its size from its own file system. */
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

int32_t sys__llseek(isr_params_t *params)
{
    int fd = (int)params->ebx;
    uint32_t offset_high = params->ecx;
    uint32_t offset_low = params->edx;
    long long __user *result = (long long __user *)params->esi;
    int whence = (int)params->edi;
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
    if (entry->type == FD_TYPE_PIPE) {
        return -ESPIPE; /* a pipe is not seekable (POSIX) */
    }
    if (entry->type == FD_TYPE_DEVICE) {
        uint64_t raw_size;

        if (devfs_fd_is_raw(entry)) {
            uint64_t resolved;
            int32_t rc;

            fd_ofd_lock(entry->ofd);
            if (!devfs_fd_raw_size(entry, &raw_size)) {
                fd_ofd_unlock(entry->ofd);
                return -ENXIO;
            }
            off = (long long)(((uint64_t)offset_high << 32) | (uint64_t)offset_low);
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
        if (devfs_fd_log_size(entry, NULL)) {
            return llseek_bootlog(entry, off_from(offset_high, offset_low), whence, result);
        }
        if (devfs_lseek_zero(entry)) {
            long long zero = 0;
            entry->ofd->offset = 0;
            if (result && copy_to_user(result, &zero, sizeof(zero)) != 0) {
                return -EFAULT;
            }
            return 0;
        }
        return -ESPIPE;
    }
    if (entry->type == FD_TYPE_CONSOLE || entry->type == FD_TYPE_MOUSE) {
        return -ESPIPE;
    }
    if (seeks_listing(entry)) {
        return llseek_listing(entry, off_from(offset_high, offset_low), whence, result);
    }
    if (entry->type != FD_TYPE_FILE || !entry->ofd) {
        return -EBADF;
    }
    seek_rc = file_size_now(entry, &size);
    if (seek_rc != 0) {
        return seek_rc;
    }

    /* The result pointer is mandatory: a NULL one is a caller bug, not a silent
     * success (musl always passes a valid loff_t*). */
    if (!result) {
        return -EFAULT;
    }
    off = (long long)(((uint64_t)offset_high << 32) | (uint64_t)offset_low);
    /* Under the description's lock, as read and write are: the offset is
     * shared by every descriptor a dup or a fork made, and a seek from
     * the current position that read it outside the lock would move it
     * from a place another process had already left. */
    fd_ofd_lock(entry->ofd);
    if (whence == 2) {
        /* The end is read again here, inside the lock. Another process on
         * this description can extend the file while this call waits for
         * it, and an end taken before the wait lands inside what that
         * process wrote. */
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

/**
 * The refusals pread64 and pwrite64 share, in Linux's order: a negative
 * offset is -EINVAL before the descriptor is looked at, a descriptor that is
 * not open or is an O_PATH one is -EBADF, one that cannot seek is -ESPIPE,
 * one not open for the direction (`write` or not) is -EBADF, a count or an
 * end past the signed range is -EINVAL, and a directory is -EISDIR.
 *
 * What cannot seek is a pipe, a socket, a terminal or pseudoterminal, the
 * mouse, the framebuffer, and a device other than a raw disk window, the log
 * and the three whose seek always lands at 0, /dev/null, /dev/zero and
 * /dev/full. A /proc snapshot is read at an offset, as Linux reads one.
 * Returns 0 with the descriptor in `*out`, or the errno.
 */
static int32_t pio_entry(int fd, int write, uint32_t count, long long pos, fd_entry_t **out)
{
    fd_entry_t *entry;

    if (pos < 0) {
        return -EINVAL;
    }
    if (!current_proc) {
        return -ESRCH;
    }
    entry = fd_get(current_proc->fds, fd);
    if (!entry || (entry->flags & FD_FLAG_PATH)) {
        return -EBADF;
    }
    if (entry->type == FD_TYPE_DEVICE) {
        if (!devfs_fd_is_raw(entry) && !devfs_fd_log_size(entry, NULL) &&
            !devfs_lseek_zero(entry)) {
            return -ESPIPE;
        }
    } else if (entry->type != FD_TYPE_FILE && entry->type != FD_TYPE_DIR &&
               entry->type != FD_TYPE_SYNTH_DIR && entry->type != FD_TYPE_PROC) {
        return -ESPIPE;
    }
    if (!(entry->flags & (write ? FD_FLAG_WRITE : FD_FLAG_READ))) {
        return -EBADF;
    }
    if (count > 0x7FFFFFFFu || (uint64_t)count > 0x7FFFFFFFFFFFFFFFull - (uint64_t)pos) {
        return -EINVAL;
    }
    if (entry->type == FD_TYPE_DIR || entry->type == FD_TYPE_SYNTH_DIR) {
        return -EISDIR;
    }
    *out = entry;
    return 0;
}

/**
 * Make `view` a copy of the device or /proc descriptor `entry` whose
 * description is `desc`, a copy of the shared one with both offsets at `at`,
 * for pread64 and pwrite64.
 *
 * A device's read or write moves the offset it uses, a raw window's own or
 * the description's for the log and a /proc snapshot, and reads nothing else
 * of the description. Given the copy, it uses and moves the copy's, and the
 * shared description, which a sharer may be reading or seeking at the same
 * time, never changes. The log and a snapshot take a 32-bit offset and are
 * shorter than 4 GiB, so a position past that is their end.
 */
static void pio_view(fd_entry_t *entry, uint64_t at, fd_entry_t *view, struct open_file *desc)
{
    *desc = *entry->ofd;
    desc->offset = at > 0xFFFFFFFFu ? 0xFFFFFFFFu : at;
    desc->raw_offset = at;
    *view = *entry;
    view->ofd = desc;
}

/**
 * pread64(2): EBX = fd, ECX = buffer, EDX = count, ESI and EDI = the
 * offset's low and high words, the i386 order. Reads at the offset and
 * leaves the descriptor's own where it was; `pio_entry` has what it refuses.
 * A regular file is read at the offset under the description's lock, as
 * read(2) reads it, and a device or a /proc snapshot through `pio_view`.
 */
int32_t sys_pread64(isr_params_t *params)
{
    char __user *buf = (char __user *)params->ecx;
    uint32_t count = params->edx;
    long long pos = off_from(params->edi, params->esi);
    uint64_t at = (uint64_t)pos;
    fd_entry_t *entry = NULL;
    fd_entry_t view;
    struct open_file desc;
    int32_t rc = pio_entry((int)params->ebx, 0, count, pos, &entry);

    if (rc != 0 || count == 0) {
        return rc;
    }
    if (entry->type == FD_TYPE_FILE) {
        return file_read(entry, buf, count, &at);
    }
    if (!entry->ops || !entry->ops->read || !entry->ofd) {
        return -EBADF;
    }
    pio_view(entry, at, &view, &desc);
    return entry->ops->read(&view, buf, count);
}

/**
 * pwrite64(2): pread64's arguments, with the buffer written from. Writes at
 * the offset and leaves the descriptor's own where it was, except that
 * O_APPEND writes at the end of the file, as Linux's pwrite does; the
 * set-id bits and the loop device's file are write's rules (`file_write`).
 * A device is written as pread64 reads it.
 */
int32_t sys_pwrite64(isr_params_t *params)
{
    const char __user *buf = (const char __user *)params->ecx;
    uint32_t count = params->edx;
    long long pos = off_from(params->edi, params->esi);
    uint64_t at = (uint64_t)pos;
    fd_entry_t *entry = NULL;
    fd_entry_t view;
    struct open_file desc;
    int32_t rc = pio_entry((int)params->ebx, 1, count, pos, &entry);

    if (rc != 0 || count == 0) {
        return rc;
    }
    if (entry->type == FD_TYPE_FILE) {
        return file_write(entry, buf, count, &at);
    }
    if (!entry->ops || !entry->ops->write || !entry->ofd) {
        return -EBADF;
    }
    pio_view(entry, at, &view, &desc);
    return entry->ops->write(&view, buf, count);
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
static int32_t flock_read(uint32_t arg, int wide, struct flock_args *fl)
{
    uint8_t raw[24];
    int32_t s32, l32;

    if (copy_from_user(raw, (const void __user *)arg, wide ? 24u : 16u) != 0) {
        return -EFAULT;
    }
    k_memcpy(&fl->type, raw, 2);
    k_memcpy(&fl->whence, raw + 2, 2);
    if (wide) {
        k_memcpy(&fl->start, raw + 4, 8);
        k_memcpy(&fl->len, raw + 12, 8);
        k_memcpy(&fl->pid, raw + 20, 4);
        return 0;
    }
    k_memcpy(&s32, raw + 4, 4);
    k_memcpy(&l32, raw + 8, 4);
    k_memcpy(&fl->pid, raw + 12, 4);
    fl->start = s32;
    fl->len = l32;
    return 0;
}

/**
 * Write F_GETLK's answer, the lock `req` found, over the caller's lock
 * description at `arg`, in the layout `flock_read` read: its type, its range
 * from the start of the file with a length of 0 for one to the end, and its
 * owner's pid; or F_UNLCK in `l_type` alone when none conflicts. Returns 0,
 * -EFAULT, or -EOVERFLOW for a lock struct flock cannot describe.
 */
static int32_t flock_write(uint32_t arg, int wide, const struct lock_req *req)
{
    uint8_t raw[24];
    int16_t type = (int16_t)req->type, whence = 0;
    int64_t start = (int64_t)req->start;
    int64_t len = req->end == LOCK_EOF ? 0 : (int64_t)(req->end - req->start + 1u);
    int32_t s32 = (int32_t)start, l32 = (int32_t)len;
    uint32_t size = wide ? 24u : 16u;

    k_memcpy(raw, &type, 2);
    if (req->type == F_UNLCK) {
        size = 2;
    } else if (!wide &&
               (req->start > 0x7FFFFFFFu || (req->end != LOCK_EOF && req->end > 0x7FFFFFFFu))) {
        return -EOVERFLOW;
    }
    k_memcpy(raw + 2, &whence, 2);
    if (wide) {
        k_memcpy(raw + 4, &start, 8);
        k_memcpy(raw + 12, &len, 8);
        k_memcpy(raw + 20, &req->pid, 4);
    } else {
        k_memcpy(raw + 4, &s32, 4);
        k_memcpy(raw + 8, &l32, 4);
        k_memcpy(raw + 12, &req->pid, 4);
    }
    return copy_to_user((void __user *)arg, raw, size) != 0 ? -EFAULT : 0;
}

/**
 * The record-lock commands of fcntl64 on `entry`: `cmd` one of the nine,
 * `arg` the caller's struct flock, 16 bytes with 32-bit offsets for F_GETLK,
 * F_SETLK and F_SETLKW, or struct flock64, 24 bytes, for the rest.
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
 */
static int32_t fcntl_lock(fd_entry_t *entry, int cmd, uint32_t arg)
{
    int wide, by_ofd;
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
    rc = flock_range(entry, fl.whence, fl.start, fl.len, &req);
    if (rc != 0) {
        return rc;
    }
    req.type = fl.type;
    req.pid = 0;
    fd_lock_key(entry, &key);
    rc = lock_record(&key, by_ofd ? entry->ofd : NULL, op, &req);
    if (rc == 0 && op != LOCK_GET && req.type != F_UNLCK && !by_ofd &&
        (entry->flags & FD_FLAG_CLOSING)) {
        fd_lock_key(entry, &key);
        lock_close(&key);
        return -EBADF;
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
 * A descriptor of anything else is EINVAL; adding needs a descriptor open
 * for writing (EPERM), then `arg` must be seals (EINVAL) and the file not
 * sealed against sealing (EPERM), in Linux's order. F_SEAL_WRITE does not
 * wait for writable shared mappings to go, as Linux's does, and is not
 * refused with EBUSY while one exists.
 */
static int32_t fcntl_seals(const fd_entry_t *entry, int cmd, uint32_t arg)
{
    if (entry->type != FD_TYPE_FILE || !entry->ofd || !entry->ofd->vmnt ||
        entry->ofd->vmnt->type != &vfs_tmpfs_type) {
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

/**
 * The descriptor-to-descriptor copies: splice (313) and tee (315), with the
 * descriptors in EBX and EDX and in EBX and ECX, and sendfile (187) and
 * sendfile64 (239), with the output descriptor in EBX and the input in ECX.
 *
 * The kernel moves no data between two descriptors itself, so each answers
 * -EINVAL once both descriptors are open, as Linux answers for descriptors it
 * cannot splice or send from, and -EBADF otherwise. GNU grep drains a pipe
 * it has read enough of with splice when its output is /dev/null, and copies
 * through read and write after an EINVAL, which it takes for "not this kind
 * of descriptor", where any other error is its read's.
 */
static int32_t no_copy(int a, int b)
{
    if (!current_proc) {
        return -ESRCH;
    }
    if (!fd_get(current_proc->fds, a) || !fd_get(current_proc->fds, b)) {
        return -EBADF;
    }
    return -EINVAL;
}

int32_t sys_splice(isr_params_t *params)
{
    return no_copy((int)params->ebx, (int)params->edx);
}

int32_t sys_tee(isr_params_t *params)
{
    return no_copy((int)params->ebx, (int)params->ecx);
}

int32_t sys_sendfile(isr_params_t *params)
{
    return no_copy((int)params->ebx, (int)params->ecx);
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

/** Whether Linux takes fallocate mode `mode` (vfs_fallocate): one mode at
 * most, a hole punched with the size kept, and a range collapsed, inserted
 * or zeroed by writes without it. A mode it does not take is -EOPNOTSUPP. */
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
 * fallocate: a file of a mounted file system is regular, a pipe a FIFO, a
 * /proc file regular, a device as devfs gives its mode, and anything else,
 * a socket or a terminal among them, no file at all (0). */
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
 * fallocate (324): EBX = fd, ECX = mode, EDX:ESI = the offset and EDI:EBP the
 * length, each low word first.
 *
 * No file system here reserves a file's blocks ahead of its writes, as
 * Linux's ext2 does not, so a call on a regular file or a block device open
 * for writing is -EOPNOTSUPP, Linux's answer for a file without the
 * operation. glibc's posix_fallocate then writes the range itself, as it
 * does on Linux's ext2; it does not after -ENOSYS. The checks before it are
 * Linux's, in its order (ksys_fallocate, vfs_fallocate): -EBADF for a
 * descriptor that is not open, -EINVAL for a negative offset or a length
 * that is not positive, -EOPNOTSUPP for a mode it does not take, -EBADF for
 * a descriptor not open for writing, then by the file's type -ESPIPE for a
 * pipe, -EISDIR for a directory and -ENODEV for anything but a regular file
 * or a block device, and -EFBIG for a range past the largest offset.
 */
int32_t sys_fallocate(isr_params_t *params)
{
    uint64_t offset = (uint64_t)params->edx | ((uint64_t)params->esi << 32);
    uint64_t len = (uint64_t)params->edi | ((uint64_t)params->ebp << 32);
    fd_entry_t *entry;
    uint32_t type;

    if (!current_proc) {
        return -ESRCH;
    }
    entry = fd_get(current_proc->fds, (int)params->ebx);
    if (!entry || (entry->flags & FD_FLAG_PATH)) {
        return -EBADF;
    }
    if ((offset >> 63) != 0 || len == 0 || (len >> 63) != 0) {
        return -EINVAL;
    }
    if (!fallocate_mode_ok(params->ecx)) {
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
    return -EOPNOTSUPP;
}

/* The longest memfd name: Linux's NAME_MAX less the "memfd:" of its link. */
#define MFD_NAME_MAX 249

/**
 * memfd_create(2): EBX = name, ECX = flags.
 *
 * A file in memory that no path names, on the tmpfs `vfs_memfd_mount` has,
 * open for reading and writing on a new descriptor: mode 0777 and the
 * caller's owner, 0666 with MFD_NOEXEC_SEAL; sealed against sealing unless
 * MFD_ALLOW_SEALING or MFD_NOEXEC_SEAL, and sealed against a change of its
 * execute bits with MFD_NOEXEC_SEAL; close-on-exec with MFD_CLOEXEC. It goes
 * with its descriptions' last close, and /proc/PID/fd names it
 * "/memfd:NAME (deleted)". A name longer than 249 bytes, MFD_HUGETLB or
 * another unknown flag, and MFD_NOEXEC_SEAL with MFD_EXEC are EINVAL.
 */
int32_t sys_memfd_create(isr_params_t *params)
{
    uint32_t flags = params->ecx;
    struct vfs_mount *mnt = vfs_memfd_mount();
    uint32_t mode = 0777u, seals = TMPFS_SEAL_SEAL;
    char name[MFD_NAME_MAX + 2];
    struct vfs_stat st;
    fd_entry_t *e;
    void *node;
    int32_t rc;
    int fd;

    if (!current_proc) {
        return -ESRCH;
    }
    if ((flags & ~(MFD_CLOEXEC | MFD_ALLOW_SEALING | MFD_NOEXEC_SEAL | MFD_EXEC)) ||
        ((flags & MFD_NOEXEC_SEAL) && (flags & MFD_EXEC))) {
        return -EINVAL;
    }
    rc = strncpy_from_user(name, (const char __user *)params->ebx, sizeof(name));
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
    fd = fd_alloc(current_proc->fds);
    if (fd < 0 || vfs_open_node(mnt, node, 2, &current_proc->fds->entries[fd].ofd->fhandle) != 0) {
        if (fd >= 0) {
            fd_free(current_proc->fds, fd);
        }
        mnt->type->ops->put(mnt, node);
        return fd < 0 ? fd : -ENOMEM;
    }
    e = &current_proc->fds->entries[fd];
    e->type = FD_TYPE_FILE;
    e->node = NULL;
    e->flags = FD_FLAG_READ | FD_FLAG_WRITE | ((flags & MFD_CLOEXEC) ? FD_FLAG_CLOEXEC : 0u);
    e->ofd->vmnt = mnt;
    e->ofd->vnode = node;
    e->ofd->ino = vfs_getattr(mnt, node, &st) == 0 ? st.ino : 0;
    vfs_desc_open(mnt);
    return fd;
}

/* flock(2)'s obsolete mandatory-lock bit, which Linux accepts and ignores. */
#define LOCK_MAND 32

/**
 * flock(2): EBX = fd, ECX = operation.
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
 */
int32_t sys_flock(isr_params_t *params)
{
    int op = (int)params->ecx;
    int kind = op & ~LOCK_NB;
    fd_entry_t *e;
    struct lock_key key;

    if (op & LOCK_MAND) {
        return 0;
    }
    if (kind != LOCK_SH && kind != LOCK_EX && kind != LOCK_UN) {
        return -EINVAL;
    }
    if (!current_proc) {
        return -ESRCH;
    }
    e = fd_get(current_proc->fds, (int)params->ebx);
    if (!e || !e->ofd || (e->flags & FD_FLAG_PATH) ||
        (kind != LOCK_UN && !(e->flags & (FD_FLAG_READ | FD_FLAG_WRITE)))) {
        return -EBADF;
    }
    fd_lock_key(e, &key);
    return lock_flock(&key, e->ofd, op);
}

/**
 * Descriptor control (fcntl64, syscall 221).
 *
 * EBX=fd, ECX=cmd, EDX=arg. Supports the descriptor-flag and status-flag
 * round-trips libc needs: F_DUPFD(_CLOEXEC), F_GETFD/F_SETFD (the
 * close-on-exec bit), and F_GETFL/F_SETFL (access + O_NONBLOCK/O_APPEND on
 * the shared open-file description, which the write and socket paths read).
 * F_GETPIPE_SZ gives a pipe's capacity, which bash asks before it writes a
 * here-document into a pipe it reads itself. An O_PATH descriptor takes the
 * commands Linux lets it take, the two duplications, F_GETFD, F_SETFD and
 * F_GETFL, which reports O_PATH, and is -EBADF to the rest.
 */
int32_t sys_fcntl64(isr_params_t *params)
{
    int fd = (int)params->ebx;
    int cmd = (int)params->ecx;
    uint32_t arg = params->edx;
    fd_entry_t *entry;

    enum {
        F_DUPFD = 0,
        F_GETFD = 1,
        F_SETFD = 2,
        F_GETFL = 3,
        F_SETFL = 4,
        F_DUPFD_CLOEXEC = 1030,
        F_GETPIPE_SZ = 1032
    };

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

    switch (cmd) {
    case F_GETFD:
        return (entry->flags & FD_FLAG_CLOEXEC) ? 1 : 0; /* 1 == FD_CLOEXEC */
    case F_SETFD:
        if (arg & 1) {
            entry->flags |= FD_FLAG_CLOEXEC;
        } else {
            entry->flags &= ~(uint32_t)FD_FLAG_CLOEXEC;
        }
        return 0;
    case F_GETFL: {
        /* Report the access mode (O_RDONLY=0/O_WRONLY=1/O_RDWR=2) plus the
         * OFD status flags we track (O_APPEND, O_NONBLOCK). */
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
            fl |= (int)(entry->ofd->status_flags & (OFD_APPEND | OFD_NONBLOCK));
        }
        if (entry->flags & FD_FLAG_PATH) {
            fl |= O_PATH;
        }
        return fl;
    }
    case F_SETFL:
        /* O_APPEND and O_NONBLOCK are the settable status flags; they live
         * on the open-file description (shared by dup/fork), not the
         * descriptor. Other bits in arg (access mode, O_CLOEXEC) are
         * accepted and ignored per POSIX. */
        if (entry->ofd) {
            entry->ofd->status_flags = arg & (OFD_APPEND | OFD_NONBLOCK);
        }
        return 0;
    case F_DUPFD:
    case F_DUPFD_CLOEXEC: {
        int newfd;
        fd_entry_t *dst;
        /* F_DUPFD duplicates onto the LOWEST free fd >= arg (the POSIX
         * contract). An arg past the descriptor limit is -EINVAL, since the
         * table grows to it and no higher; no slot at or above it is
         * -EMFILE, and no room in the heap -ENOMEM. */
        if (arg >= (uint32_t)fd_limit(current_proc->fds)) {
            return -EINVAL;
        }
        newfd = fd_alloc_from(current_proc->fds, (int)arg);
        if (newfd < 0) {
            return newfd;
        }
        entry = fd_get(current_proc->fds, fd); /* the entries may have moved */
        dst = &current_proc->fds->entries[newfd];
        /* The duplicate SHARES oldfd's open-file description (so the seek
         * offset is shared, per POSIX). fd_alloc_from just gave dst a fresh
         * OFD; drop it and point dst at oldfd's, taking a reference. A
         * shared socket gains a reference too; FD_CLOEXEC is
         * per-descriptor. */
        fd_ofd_unref(dst->ofd);
        dst->type = entry->type;
        dst->node = entry->node;
        dst->sock = entry->sock;
        dst->ops = entry->ops; /* share the backing object + vtable */
        dst->priv = entry->priv;
        dst->dev = entry->dev;
        dst->ofd = entry->ofd;
        fd_ofd_ref(dst->ofd);
        dst->flags = entry->flags;
        if (entry->type == FD_TYPE_SOCKET && entry->sock) {
            socket_ref(entry->sock);
        }
        if (cmd == F_DUPFD_CLOEXEC) {
            dst->flags |= FD_FLAG_CLOEXEC;
        } else {
            dst->flags &= ~(uint32_t)FD_FLAG_CLOEXEC;
        }
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
        return fcntl_seals(entry, cmd, arg);
    case F_GETPIPE_SZ:
        return entry->type == FD_TYPE_PIPE ? PIPE_BUF_SIZE : -EBADF;
    default:
        return -EINVAL;
    }
}

/**
 * Copy the three times out of a stat64 into the narrower old stat.
 *
 * The old ABI is a different struct, so every path that fills it fills it
 * separately; before this kernel had a clock they were all zero and the
 * difference did not show.
 */
static void stat_times_from_stat64(struct linux_stat *kst, const struct linux_stat64 *s64)
{
    kst->st_atime = s64->st_atime;
    kst->st_atime_nsec = s64->st_atime_nsec;
    kst->st_mtime = s64->st_mtime;
    kst->st_mtime_nsec = s64->st_mtime_nsec;
    kst->st_ctime = s64->st_ctime;
    kst->st_ctime_nsec = s64->st_ctime_nsec;
}

/** The three times of a KRFFS node, into the old stat. */
static void stat_node_times(struct linux_stat *kst, const krffs_node_t *node)
{
    struct linux_stat64 s64;

    stat64_node_times(&s64, node);
    stat_times_from_stat64(kst, &s64);
}

/** Copy the old stat `kst` out to `st`, or answer -ENOTDIR when the path
 * named a directory by its end (`path_names_dir`) and reached something
 * else, as Linux's walk does. */
static int32_t old_stat_put(struct linux_stat __user *st, const struct linux_stat *kst, int dir)
{
    if (dir && (kst->st_mode & S_IFMT) != S_IFDIR) {
        return -ENOTDIR;
    }
    return copy_to_user(st, kst, sizeof(*kst)) != 0 ? -EFAULT : 0;
}

/** The root directory's three times, into the old stat. */
static void stat_root_times(struct linux_stat *kst)
{
    struct linux_stat64 s64;

    stat64_root_times(&s64);
    stat_times_from_stat64(kst, &s64);
}

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

    /* Build the result in a kernel struct, then copy it out in one shot. */
    k_memset(&kst, 0, sizeof(kst));

    if (names_tree_root(pathbuf)) {
        path_buf_put(pathbuf);
        kst.st_mode = S_IFDIR | 0755; /* drwxr-xr-x */
        kst.st_nlink = 1;
        kst.st_ino = 1;
        kst.st_blksize = 4096;
        /* The root is a real directory with real times: an entry created in it
         * stamps them, which is how a program watching a directory sees it. */
        stat_root_times(&kst);
        if (copy_to_user(st, &kst, sizeof(kst)) != 0) {
            return -EFAULT;
        }
        return 0;
    }

    /* /dev and /proc are synthetic. The old stat(2) ABI is a narrower struct,
     * so translate the synthetic stat64 result down into it. The walk keeps
     * its mount's lock until the node is read, as `do_stat64`'s does. */
    {
        struct vfs_ref ref;
        const struct vpath *svp;
        int32_t rc =
            vfs_resolve(0, pathbuf, VFS_WALK_LOCK | (follow ? 0 : VFS_WALK_NOFOLLOW), &ref, 0, 0);

        path_buf_put(pathbuf);
        if (rc != 0) {
            return rc; /* missing, not-a-dir, or a denied ancestor */
        }
        svp = vfs_synth_vpath(ref.mnt, ref.node);
        if (svp) {
            struct linux_stat64 s64;

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
            struct linux_stat64 s64;

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
        kst.st_nlink = 1; /* Always 1 for KRFFS */
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

/* stat(106) follows symlinks; lstat(107) does not. musl maps SYS_stat and
 * SYS_lstat to the 64-bit forms, 195 and 196, so these two serve a caller
 * that issues the old numbers itself. */
int32_t sys_stat(isr_params_t *params)
{
    return do_old_stat((const char __user *)params->ebx, (struct linux_stat __user *)params->ecx,
                       1);
}

int32_t sys_lstat(isr_params_t *params)
{
    return do_old_stat((const char __user *)params->ebx, (struct linux_stat __user *)params->ecx,
                       0);
}

/*
 * creat(2): EBX = path, ECX = mode. Returns an fd open for writing, or
 * -errno. An existing regular file is truncated to zero by the call; a
 * directory, the root included, is EISDIR.
 */
int32_t sys_creat(isr_params_t *params)
{
    const char __user *path = (const char __user *)params->ebx;
    uint32_t mode = params->ecx;
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
    /* creat(2) is open(O_WRONLY|O_CREAT|O_TRUNC), and it is exactly that
     * call, so the two cannot drift: the EISDIR for a directory, the write
     * check, the truncate with its set-id clear, and the stamp of a new
     * node all happen where open does them. */
    rc = open_named(path_name(pathbuf), 1 /* O_WRONLY */ | O_CREAT | O_TRUNC, mode, end);
    path_buf_put(pathbuf);
    return rc;
}

/**
 * Remove the name `leaf` from the KRFFS root's directory `parent`: the
 * root's half of `unlink_named`, run under the root's namespace lock, which
 * the caller took with its walk and keeps until this returns.
 *
 * The directory's write bit is checked before the name is looked up, so a
 * caller who may not write there sees -EACCES and does not learn whether
 * the name exists. The root and a directory are -EISDIR, and anything else
 * -ENOTDIR when the path had a slash after the name (`slash`); the sticky
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
 * (`path_names_dir`).
 *
 * Every refusal is decided here, not below, because `krffs_unlink`
 * answers all of them with a bare -1, which the wrapper reports as `EIO`
 * whatever went wrong. A missing component gives `-ENOENT` and a plain file
 * in the middle of the path `-ENOTDIR`, both from the reporting resolver;
 * the root and any directory give `-EISDIR`, which is what tells a caller
 * to use `rmdir`. The root is tested by node identity because it has
 * 0755 with no type bits and would otherwise read as a plain file. A path
 * whose last name is "." or ".." names a directory, -EISDIR, and one with a
 * slash after its last name, `end` PATH_END_SLASH, names one too: a name
 * there that is not a directory, a symbolic link to one among them, is
 * -ENOTDIR, as on Linux.
 *
 * The walk keeps the parent's namespace lock, where its type takes one,
 * until the name is gone: on the KRFFS root the holder count of
 * `unlink_root_name` is right only under that lock, which an open holds
 * until it has published its descriptor.
 */
static int32_t unlink_named(const char *path, int end)
{
    const char *name;
    struct vfs_ref parent;
    char leaf[VFS_NAME_MAX + 1];
    int32_t rc;

    if (path[0] == '/' && path[1] == '\0') {
        return -EISDIR; /* the root directory cannot be unlinked */
    }

    name = path;
    if (name[0] == '/') {
        name++;
    }

    if (name[0] == '\0') {
        return -EINVAL; /* empty filename */
    }
    rc = vfs_resolve(0, path, VFS_WALK_PARENT | VFS_WALK_LOCK, &parent, leaf, sizeof(leaf));
    if (rc != 0) {
        return rc;
    }
    if (leaf_is_dot(leaf)) {
        rc = -EISDIR;
    } else if (vfs_synth_vpath(parent.mnt, parent.node)) {
        rc = -EROFS; /* the synthetic mounts are read-only */
    } else if (vfs_leaf_mount(parent.mnt, parent.node, leaf)) {
        rc = -EBUSY; /* a mount point cannot be unlinked */
    } else if (mnt_is_root(parent.mnt)) {
        rc = unlink_root_name(&parent, leaf, end == PATH_END_SLASH);
    } else {
        vfs_ns_lock(parent.mnt);
        if (end == PATH_END_SLASH) {
            rc = leaf_dir_check(&parent, leaf);
            rc = rc == 0 ? -EISDIR : rc;
        } else {
            rc = cred_remove_ref(&parent, leaf);
            if (rc == 0) {
                rc = vfs_unlink_name(parent.mnt, parent.node, leaf);
            }
        }
        vfs_ns_unlock(parent.mnt);
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

/* unlink(2): EBX = path. Returns 0 or -errno. */
int32_t sys_unlink(isr_params_t *params)
{
    const char __user *upath = (const char __user *)params->ebx;
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
    uint32_t written;
};

/** The core file's writer for `core_write`: `len` bytes of `buf` at the
 * file's place, or a hole of `len` bytes for a null `buf`. A short write is
 * a full disk. */
static int32_t core_file_put(void *ctx, const void *buf, uint32_t len)
{
    struct core_file *cf = ctx;
    int32_t n;

    if (buf) {
        n = vfs_write(cf->ofd->vmnt, cf->ofd->fhandle, buf, len, cf->pos);
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
int32_t fs_core_file(int signo, uint32_t limit)
{
    static const char zero;
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
    entry = &current_proc->fds->entries[fd];
    entry->flags |= FD_FLAG_CLOSING | FD_FLAG_RELEASING;
    if (!entry->ofd || !entry->ofd->vmnt) {
        rc = -EINVAL;
    } else {
        cf.ofd = entry->ofd;
        cf.pos = 0;
        cf.end = 0;
        cf.written = 0;
        rc = core_write(core_file_put, &cf, notes, notes_size, limit);
        if (rc == 0 && cf.end < cf.pos) {
            cf.pos--;
            rc = cf.written < limit ? core_file_put(&cf, &zero, 1) : -EFBIG;
        }
    }
    kfree(notes);
    fd_release_backing(entry);
    fd_free(current_proc->fds, fd);
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
 * in /dev or /proc is -EEXIST too, before the -EROFS of a name that does
 * not, as on Linux.
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
        return -EROFS; /* nothing is created under /dev or /proc */
    }
    if (!mnt_is_root(parent->mnt)) {
        rc = cred_access_ref(parent, CRED_W, 0);
        if (rc == 0) {
            vfs_ns_lock(parent->mnt);
            rc = vfs_mkdir(parent->mnt, parent->node, leaf,
                           mode & 07777u & ~current_proc->fs->umask);
            vfs_ns_unlock(parent->mnt);
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
 * Create the directory `path`, an absolute canonical path, with the
 * permission bits of `mode` less the umask, as any creation takes them: the
 * core of mkdir(2) and mkdirat(2). Returns 0 or -errno (-EEXIST if it exists,
 * a last name of "." or ".." among them, -ENOENT if a parent is missing).
 * The walk keeps the parent's namespace lock, where its type takes one,
 * until the directory is made (`mkdir_in`).
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
        return -EEXIST; /* the root exists; mkdir -p walks through here */
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

/* mkdir(2): EBX = path, ECX = mode. */
int32_t sys_mkdir(isr_params_t *params)
{
    const char __user *upath = (const char __user *)params->ebx;
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
    rc = do_mkdir(path, params->ecx);
    path_buf_put(path);
    return rc;
}

/* mkdirat(2): EBX = dirfd, ECX = path, EDX = mode; a relative path is
 * resolved from `dirfd` as openat's is. */
int32_t sys_mkdirat(isr_params_t *params)
{
    const char __user *upath = (const char __user *)params->ecx;
    char *path;
    int end;
    int32_t rc;

    if (!current_proc || !upath) {
        return -EFAULT;
    }
    rc = path_from_user_dir((int)params->ebx, upath, &path, &end);
    if (rc != 0) {
        return rc;
    }
    rc = do_mkdir(path, params->edx);
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
 * `mknod_kind` has passed: the core of mknod(2) and mknodat(2).
 *
 * The kind is the S_IF type bits of `mode`, a regular file for none; `dev`
 * is a device's number as the call takes it, Linux's 32-bit encoding, which
 * is the kernel's own. The refusals after the kind's and the path's come in
 * Linux's order: the root (EEXIST, `new_name_parent`), a path that named a
 * directory by its end, `end`, as `make_dir_end` answers it, a name that
 * exists (EEXIST, a symbolic link included, `new_name_taken`), then /dev and
 * /proc, where nothing is made (EROFS), a read-only mount, the write
 * permission on the directory, and last a device node made by a caller who
 * is not root (EPERM). A file system that cannot keep the kind answers
 * EPERM.
 */
static int32_t do_mknod(const char *path, uint32_t mode, uint32_t dev, int end)
{
    uint32_t type = mode & S_IFMT;
    struct vfs_ref parent;
    char leaf[VFS_NAME_MAX + 1];
    void *made;
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
        return -EROFS; /* nothing is created under /dev or /proc */
    }
    if (mnt_is_root(parent.mnt)) {
        rc = cred_access_node((krffs_node_t *)parent.node, CRED_W, 0);
    } else if (parent.mnt->flags & VFS_MS_RDONLY) {
        rc = -EROFS;
    } else {
        rc = cred_access_ref(&parent, CRED_W, 0);
    }
    if (rc == 0 && (type == S_IFCHR || type == S_IFBLK) && !cred_privileged()) {
        rc = -EPERM;
    }
    if (rc == 0) {
        vfs_ns_lock(parent.mnt);
        rc = node_make(&parent, leaf, mode, dev, &made);
        vfs_ns_unlock(parent.mnt);
        if (rc == 0 && parent.mnt->type->ops->put) {
            parent.mnt->type->ops->put(parent.mnt, made);
        }
    }
    vfs_ref_release(&parent);
    return rc;
}

/* mknod(2): EBX = path, ECX = mode, EDX = dev. The kind is checked before
 * the path is read. */
int32_t sys_mknod(isr_params_t *params)
{
    const char __user *upath = (const char __user *)params->ebx;
    char *path;
    int end;
    int32_t rc;

    if (!current_proc) {
        return -EFAULT;
    }
    rc = mknod_kind(params->ecx);
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
    rc = do_mknod(path, params->ecx, params->edx, end);
    path_buf_put(path);
    return rc;
}

/* mknodat(2): EBX = dirfd, ECX = path, EDX = mode, ESI = dev; a relative
 * path is resolved from `dirfd` as openat's is, after the kind is checked. */
int32_t sys_mknodat(isr_params_t *params)
{
    const char __user *upath = (const char __user *)params->ecx;
    char *path;
    int end;
    int32_t rc;

    if (!current_proc) {
        return -EFAULT;
    }
    rc = mknod_kind(params->edx);
    if (rc != 0) {
        return rc;
    }
    if (!upath) {
        return -EFAULT;
    }
    rc = path_from_user_dir((int)params->ebx, upath, &path, &end);
    if (rc != 0) {
        return rc;
    }
    rc = do_mknod(path, params->edx, params->esi, end);
    path_buf_put(path);
    return rc;
}

/**
 * Whether the directory `dir` of the KRFFS root on `mnt` is in use: a
 * description has it, an O_PATH descriptor's or one in flight on a socket
 * among them, or it is a process's working directory. A descriptor of the
 * root finds its directory by the node's place, which the allocator gives
 * the next node it makes once the directory is freed, so `rmdir` refuses a
 * directory in use, as a KRFFS volume and ext2 refuse theirs.
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
 * are -EBUSY.
 *
 * The walk keeps the parent's namespace lock, where its type takes one,
 * until the directory is gone: on the KRFFS root the use `rmdir_root_name`
 * finds is right only under that lock.
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
        rc = -EROFS; /* the synthetic mounts are read-only */
    } else if (vfs_leaf_mount(parent.mnt, parent.node, leaf) ||
               vfs_leaf_root(parent.mnt, parent.node, leaf)) {
        rc = -EBUSY;
    } else if (mnt_is_root(parent.mnt)) {
        rc = rmdir_root_name(&parent, leaf);
    } else {
        vfs_ns_lock(parent.mnt);
        rc = cred_remove_ref(&parent, leaf);
        if (rc == 0) {
            rc = vfs_rmdir(parent.mnt, parent.node, leaf);
        }
        vfs_ns_unlock(parent.mnt);
    }
    vfs_walk_unlock(parent.mnt);
    vfs_ref_release(&parent);
    return rc;
}

/* rmdir(2): EBX = path. */
int32_t sys_rmdir(isr_params_t *params)
{
    const char __user *upath = (const char __user *)params->ebx;
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
 * unlinkat(2): EBX = dirfd, ECX = path, EDX = flags. The name is removed as
 * unlink removes it, or as rmdir does with AT_REMOVEDIR; any other flag is
 * -EINVAL before the path is read. A relative path is resolved from `dirfd`.
 */
int32_t sys_unlinkat(isr_params_t *params)
{
    const char __user *upath = (const char __user *)params->ecx;
    int flags = (int)params->edx;
    char *path;
    int end;
    int32_t rc;

    if (flags & ~AT_REMOVEDIR) {
        return -EINVAL;
    }
    if (!current_proc || !upath) {
        return -EFAULT;
    }
    rc = path_from_user_dir((int)params->ebx, upath, &path, &end);
    if (rc != 0) {
        return rc;
    }
    rc = (flags & AT_REMOVEDIR) ? do_rmdir(path) : unlink_named(path, end);
    path_buf_put(path);
    return rc;
}

/* The renameat2 flag this kernel does: refuse to replace the new name. */
#define RENAME_NOREPLACE 1

/**
 * The refusal RENAME_NOREPLACE adds to a rename: -ENOENT for a source that is
 * not there, as the rename would answer, then -EEXIST for a new name that is,
 * a mount point being there for either, before the EBUSY a mount point is
 * otherwise, as on Linux. The caller holds the mount's namespace lock from
 * this check through the rename, and every namespace change takes that
 * lock, so no name can appear between the two.
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
 * other half of `do_rename`, which holds the mount's namespace lock from the
 * first lookup through the rename. RENAME_NOREPLACE in `flags` refuses a new
 * name that exists with -EEXIST; a source that is not a directory is
 * -ENOTDIR when either name had a slash after it, `slash`; then the removal
 * rules of `cred_remove_ref` for both names, and the type's own answers.
 */
static int32_t rename_mounted_names(const struct vfs_ref *opar, const char *oleaf,
                                    const struct vfs_ref *npar, const char *nleaf, int flags,
                                    int slash)
{
    int32_t rc;

    vfs_ns_lock(opar->mnt);
    rc = (flags & RENAME_NOREPLACE) ? rename_noreplace(opar, oleaf, npar, nleaf) : 0;
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
    vfs_ns_unlock(opar->mnt);
    return rc;
}

/**
 * Rename `old_buf` to `new_buf`, both absolute canonical paths: the core of
 * rename(2), renameat(2) and renameat2(2), whose RENAME_NOREPLACE in `flags`
 * refuses a new name that exists with -EEXIST, decided with the rename under
 * one namespace lock. The directories' write permission is checked before
 * either name is looked up, so a caller without it sees -EACCES where Linux
 * would report the lookups' -ENOENT or -EEXIST. `slash` says that either
 * path had a slash after its last name. Returns 0 or -errno.
 *
 * As in `unlink`, every refusal is decided here, because `krffs_rename`
 * answers all of them with a bare -1, which the wrapper reports as `EIO`.
 * The two directory cases are the ones POSIX separates and the memory root
 * does not (a mounted KRFFS volume answers `EIO` for both, except `EISDIR`
 * for a directory destination that is open, and ext2 moves directories): a
 * directory SOURCE is `-EPERM`, since a directory's children
 * reference its id and a move would orphan them, so the rename cannot be
 * performed at all; a directory DESTINATION under a plain source is
 * `-EISDIR`, the case that errno is reserved for. The rest follow the
 * reporting resolver: `-ENOENT` for a missing source or destination parent,
 * `-ENOTDIR` for a plain file used as a path component, `-EINVAL` for a
 * destination naming no entry.
 *
 * A last name of "." or ".." in either path, which the walk to its parent
 * returns as the name, is -EBUSY once both walks are made and the two are on
 * one mount, and in the new path -EEXIST under RENAME_NOREPLACE, as on
 * Linux, which checks these before it asks for write access: before the
 * write bits, and before the -EROFS of /dev and /proc, where nothing is
 * renamed. Either path naming the root is answered the same way, -EBUSY, or
 * -EEXIST for the new path under RENAME_NOREPLACE, before either path is
 * walked, where Linux walks both first: the path copy canonicalizes the root
 * to "/", which has no last name for a walk to the parent to give. The
 * identity tests of `rename_root_names` are therefore unreachable today and
 * are kept as defence: freeing the root as a replaced destination would take
 * the file system with it, and that guarantee should not rest on a
 * canonicalizer in another function.
 *
 * Both names are walked without the namespace lock, which is taken for the
 * lookups and the rename that follow. A walk takes the lock of each mount it
 * looks in, as ext2's lookup does, so a rename that walked its new name
 * holding the lock of the old one would hold one mount's lock while it waited
 * for another's, and two renames, or a rename and a read through a loop
 * device, could wait for each other. On the KRFFS root the holder count of
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
    /* A rename writes both directories: the entry leaves one and lands in
     * the other. Both write bits are checked before the source is even
     * looked up, as unlink orders it. */
    rc = mnt_is_root(opar.mnt) ? cred_access_node((krffs_node_t *)opar.node, CRED_W, 0)
                               : cred_access_ref(&opar, CRED_W, 0);
    if (rc == 0) {
        rc = mnt_is_root(npar.mnt) ? cred_access_node((krffs_node_t *)npar.node, CRED_W, 0)
                                   : cred_access_ref(&npar, CRED_W, 0);
    }
    if (rc == 0 && vfs_synth_vpath(opar.mnt, opar.node)) {
        rc = -EROFS; /* nothing is renamed in /dev or /proc */
    }
    if (rc != 0) {
        goto out;
    }
    if (vfs_leaf_mount(opar.mnt, opar.node, oleaf) || vfs_leaf_mount(npar.mnt, npar.node, nleaf)) {
        rc = -EBUSY; /* a mount point moves for no one */
        if (flags & RENAME_NOREPLACE) {
            vfs_ns_lock(opar.mnt);
            rc = rename_noreplace(&opar, oleaf, &npar, nleaf);
            vfs_ns_unlock(opar.mnt);
            if (rc == 0) {
                rc = -EBUSY;
            }
        }
        goto out;
    }
    if (!mnt_is_root(opar.mnt)) {
        rc = rename_mounted_names(&opar, oleaf, &npar, nleaf, flags, slash);
        goto out;
    }
    vfs_ns_lock(opar.mnt);
    rc = rename_root_names(&opar, oleaf, &npar, nleaf, flags, slash);
    vfs_ns_unlock(opar.mnt);
out:
    vfs_ref_release(&npar);
    vfs_ref_release(&opar);
    return rc;
}

/**
 * rename(2), renameat(2) and renameat2(2) after their arguments: both paths
 * are copied, each relative one joined to its own directory, before either
 * is resolved, a last "." kept as the name the walk to the parent returns
 * (`path_put_dot`).
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

/* rename(2): EBX = old path, ECX = new path. Returns 0 or -errno. */
int32_t sys_rename(isr_params_t *params)
{
    const char __user *old_path = (const char __user *)params->ebx;
    const char __user *new_path = (const char __user *)params->ecx;

    if (!current_proc || !old_path || !new_path) {
        return -EFAULT;
    }
    return rename_from_user(AT_FDCWD, old_path, AT_FDCWD, new_path, 0);
}

/* renameat(2): EBX = old dirfd, ECX = old path, EDX = new dirfd, ESI = new
 * path; each relative path is resolved from its own descriptor. */
int32_t sys_renameat(isr_params_t *params)
{
    const char __user *old_path = (const char __user *)params->ecx;
    const char __user *new_path = (const char __user *)params->esi;

    if (!current_proc || !old_path || !new_path) {
        return -EFAULT;
    }
    return rename_from_user((int)params->ebx, old_path, (int)params->edx, new_path, 0);
}

/**
 * renameat2(2): renameat's arguments and EDI = flags. RENAME_NOREPLACE
 * refuses a new name that exists with -EEXIST; RENAME_EXCHANGE,
 * RENAME_WHITEOUT and any other flag are -EINVAL before a path is read, as a
 * Linux file system without them answers them later.
 */
int32_t sys_renameat2(isr_params_t *params)
{
    const char __user *old_path = (const char __user *)params->ecx;
    const char __user *new_path = (const char __user *)params->esi;
    int flags = (int)params->edi;

    if (flags & ~RENAME_NOREPLACE) {
        return -EINVAL;
    }
    if (!current_proc || !old_path || !new_path) {
        return -EFAULT;
    }
    return rename_from_user((int)params->ebx, old_path, (int)params->edx, new_path, flags);
}

/* Special nsec values */
#define UTIME_NOW ((1L << 30) - 1L)

#define UTIME_OMIT ((1L << 30) - 2L)

/* Timer tick frequency (100 Hz) */
#define TIMER_FREQUENCY 100

/*
 * get_current_time - the current real time, in seconds and nanoseconds
 *
 * Answered by the clock (sys/rtc.c): the second the CMOS reported at boot plus
 * the ticks since that read, or time since boot on a machine whose clock could
 * not be read, either way with the PIT's place within the tick added and
 * never earlier than the reading before it (`rtc_realtime`).
 */
void get_current_time(int64_t *seconds, int64_t *nanoseconds)
{
    rtc_realtime(seconds, nanoseconds);
}

/** Whether `nsec` is a time's nanoseconds, UTIME_NOW or UTIME_OMIT. */
static int utime_nsec_valid(int32_t nsec)
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
 * one step under the mount's namespace lock, as for chmod and chown.
 */
static int32_t utimes_node(struct vfs_mount *mnt, void *node, const struct kernel_timespec *times)
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
        vfs_ns_lock(mnt);
        rc = vfs_getattr(mnt, node, &vst);
        if (rc == 0 && !cred_privileged() && current_proc->euid != vst.uid) {
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
        vfs_ns_unlock(mnt);
        return rc;
    }
    if (kn == krffs_fs->node) {
        return 0;
    }
    if (now_both) {
        if (!cred_privileged() && current_proc->euid != kn->uid &&
            cred_access_node(kn, CRED_W, 0) != 0) {
            return -EACCES;
        }
    } else if (!cred_privileged() && current_proc->euid != kn->uid) {
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
static int32_t utimes_fd(int fd, const struct kernel_timespec *times, int empty)
{
    fd_entry_t *entry = fd_get(current_proc->fds, fd);
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
    vfs_ns_lock(mnt);
    if (entry->node && fd_node_check(entry) != 0) {
        rc = -EBADF;
    } else {
        rc = utimes_node(mnt, entry->ofd->vnode, times);
    }
    vfs_ns_unlock(mnt);
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
static int32_t utimes_path(const char *path, int flags, int dir,
                           const struct kernel_timespec *times)
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
    vfs_ref_release(&ref);
    return rc;
}

/**
 * Update a file's access and modification timestamps (utimensat(2)).
 *
 * EBX = dirfd, ECX = path, EDX = times, ESI = flags. `times` is the pair
 * [atime, mtime] or NULL, as `utimes_node` takes it, and a pair of
 * UTIME_OMIT changes nothing and returns 0 before anything else is looked at,
 * as on Linux. A NULL path names the file `dirfd` refers to, with no flags
 * (-EINVAL otherwise); with AT_FDCWD it is -EFAULT. Otherwise the path
 * resolves as openat's does, AT_SYMLINK_NOFOLLOW sets a final symbolic link's
 * own times, and with AT_EMPTY_PATH an empty path names the file `dirfd`
 * refers to, or the working directory for AT_FDCWD; `utimes_path` sets the
 * times. Returns 0 or -errno.
 */
int32_t sys_utimensat(isr_params_t *params)
{
    int dirfd = (int)params->ebx;
    const char __user *user_path = (const char __user *)params->ecx;
    struct kernel_timespec __user *user_times = (struct kernel_timespec __user *)params->edx;
    int flags = (int)params->esi;
    struct kernel_timespec ktimes[2];
    const struct kernel_timespec *times = NULL;
    char *pathbuf;
    int empty = 0;
    int dir = 0;
    int32_t rc;

    if (!current_proc) {
        return -EFAULT;
    }
    if (user_times) {
        if (copy_from_user(ktimes, user_times, sizeof(ktimes)) != 0) {
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

/*
 * Linux dirent64 structure for getdents64 syscall
 */
struct linux_dirent64 {
    uint64_t d_ino;    /* 64-bit inode number */
    int64_t d_off;     /* offset to next entry */
    uint16_t d_reclen; /* Size of this dirent */
    uint8_t d_type;    /* File type */
    char d_name[1];    /* Filename (variable length, null-terminated) */
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

/* Format an unsigned decimal into out (NUL-terminated). */
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
 * The n-th entry, "." and ".." counted off, of a process's directory, its
 * task directory or its fd directory, from the directory descriptor's
 * record `sd`: fills name/ino/dtype and returns 1, or 0 past the end or
 * once the process, or the thread a thread's directory is of, is gone. A
 * thread's directory has the files of the thread with no `task` among
 * them, and its fd directory the thread's descriptors.
 */
static int synth_proc_dirent(const struct synth_dir *sd, int n, char *name, uint64_t *ino,
                             uint8_t *dtype)
{
    static const struct {
        const char *name;
        uint8_t type;
    } files[] = {{"stat", DT_REG},   {"cmdline", DT_REG}, {"fd", DT_DIR},
                 {"maps", DT_REG},   {"smaps", DT_REG},   {"status", DT_REG},
                 {"statm", DT_REG},  {"auxv", DT_REG},    {"environ", DT_REG},
                 {"comm", DT_REG},   {"mem", DT_REG},     {"oom_score_adj", DT_REG},
                 {"mounts", DT_REG}, {"limits", DT_REG},  {"exe", DT_LNK},
                 {"cwd", DT_LNK},    {"root", DT_LNK},    {"task", DT_DIR}};
    struct vpath vp;
    process_t *pp;
    int id, i, c = 0;

    synth_dir_vpath(sd, &vp);
    id = procfs_resolve_id(&vp);
    pp = id < 0 ? NULL : get_process((uint32_t)id);
    if (!pp) {
        return 0; /* the process exited: an empty directory (just . and ..) */
    }
    if (sd->kind == VNODE_PROC_PID) {
        /* `task`, the last entry, is not in a thread's directory. */
        if (n >= (int)(sizeof(files) / sizeof(files[0])) - (sd->tid != 0 ? 1 : 0)) {
            return 0;
        }
        for (i = 0; files[n].name[i]; i++) {
            name[i] = files[n].name[i];
        }
        name[i] = '\0';
        *ino = procfs_ino_pid(id);
        *dtype = files[n].type;
        return 1;
    }
    if (sd->kind == VNODE_PROC_TASK_DIR) {
        /* The process's threads, the leader first, which a thread whose end
         * is still to be reaped is not among unless it is the leader. */
        process_t *t;

        for (t = proc_list; t; t = t->next) {
            if (t->tg != pp->tg || t->state == PROC_UNUSED ||
                (t->state == PROC_ZOMBIE && t != pp->tg->leader)) {
                continue;
            }
            if (c++ == n) {
                synth_u32_str(name, t->pid);
                *ino = procfs_ino_pid((int)t->pid);
                *dtype = DT_DIR;
                return 1;
            }
        }
        return 0;
    }
    for (i = 0; i < fd_table_cap(pp->fds); i++) {
        if (pp->fds->entries[i].type != FD_TYPE_NONE &&
            !(pp->fds->entries[i].flags & FD_FLAG_CLOSING) && c++ == n) {
            synth_u32_str(name, (uint32_t)i);
            *ino = procfs_ino_pid(id);
            *dtype = DT_LNK;
            return 1;
        }
    }
    return 0;
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
        struct netdev *nd = netdev_at(n);

        if (!nd) {
            return 0;
        }
        sysfs_net_name(nd->index, name, 32);
        *ino = sysfs_ino(SYSFS_NET_IF, nd->index, 0);
        *dtype = DT_DIR;
        return 1;
    }
    for (i = 0; (attr = sysfs_net_attr(node, i)) != NULL; i++) {
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
    if (node == SYSFS_NET_IF && n == i + 1 && sysfs_net_pci_key(key) >= 0) {
        k_strncpy(name, "device", 31);
        *ino = sysfs_ino(SYSFS_PCI_DEVICE, sysfs_net_pci_key(key), 2);
        *dtype = DT_LNK;
        return 1;
    }
    return 0;
}

/**
 * The `n`th entry of the /sys directory `node`, whose device is `key`: its
 * name, inode and type, and 1, or 0 past the last. The fixed directories
 * hold the next ones down, a bus's `devices` a link for each device, the
 * directory the links lead to a directory for each, a device's directory
 * its uevent, and /sys/class/net its interfaces.
 */
static int synth_sys_dirent(int node, int key, int n, char *name, uint64_t *ino, uint8_t *dtype)
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
                 {SYSFS_ROOT, "class", SYSFS_CLASS},
                 {SYSFS_CLASS, "net", SYSFS_CLASS_NET}};
    int i, dev, links;

    for (i = 0; i < (int)(sizeof(fixed) / sizeof(fixed[0])); i++) {
        if (fixed[i].node == node && n-- == 0) {
            k_strncpy(name, fixed[i].name, 31);
            name[31] = '\0';
            *ino = sysfs_ino(fixed[i].child, 0, 0);
            *dtype = DT_DIR;
            return 1;
        }
    }
    if (node == SYSFS_CLASS_NET || node == SYSFS_NET_IF || node == SYSFS_NET_STATS) {
        return synth_sys_net_dirent(node, key, n, name, ino, dtype);
    }
    if (node == SYSFS_PCI_DEVICE || node == SYSFS_USB_DEVICE) {
        if (n != 0) {
            return 0;
        }
        k_strncpy(name, "uevent", 31);
        *ino = sysfs_ino(node, key, 1);
        *dtype = DT_REG;
        return 1;
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

/*
 * The n-th entry of a synthetic directory (its kind, pid and tid from the
 * dir fd's record `sd`): fills name/ino/dtype and returns 1, or 0 past the
 * end. "." and ".." are entries 0 and 1 of every directory.
 */
static int synth_dirent_at(const struct synth_dir *sd, int n, char *name, uint64_t *ino,
                           uint8_t *dtype)
{
    int kind = sd->kind;

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
        const char *dn = NULL;
        uint32_t dino = 0;
        uint32_t dmode = 0;
        uint64_t mino = 0;
        int i, named = 0;

        /* The mounts that stand in /dev by name come first: /dev/shm. */
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
        /* /dev/log, the link to the syslog daemon's socket. */
        if (n == 0) {
            k_strncpy(name, "log", 31);
            *ino = DEV_LOG_INO;
            *dtype = DT_LNK;
            return 1;
        }
        n--;
        /* The caller's descriptors, /dev/fd and the three standard ones
         * (`synth_classify`), with what lstat reports of them. */
        if (n < 4) {
            static const char *const fdnames[] = {"fd", "stdin", "stdout", "stderr"};
            struct linux_stat64 fst;
            struct vpath fvp;

            k_memset(&fvp, 0, sizeof(fvp));
            fvp.pid = SYNTH_PID_SELF;
            fvp.kind = n == 0 ? VNODE_PROC_FD_DIR : VNODE_PROC_FD_LINK;
            fvp.procfd = n - 1;
            for (i = 0; fdnames[n][i]; i++) {
                name[i] = fdnames[n][i];
            }
            name[i] = '\0';
            *ino = synth_stat(&fvp, &fst, 0) == 0 ? fst.st_ino : 1u;
            *dtype = n == 0 ? DT_DIR : DT_LNK;
            return 1;
        }
        n -= 4;
        /* Through devfs_dirent, which skips absent entries and reports the
         * registry inode: two devices are dynamically present now, so a walk
         * that stopped at the first absent one would hide the second. */
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
    if (kind == VNODE_PTS_DIR) {
        /* /dev/pts: one entry per allocated pty slave, named by its index. */
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
    /* A directory of /proc/sys: the entries of the table under it. */
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
    /* The files of /proc/net: dev, route, arp and tcp, then the ones known by
     * their origin, from SYNTH_PROC_NETUDP. */
    if (kind == VNODE_PROC_NET_DIR) {
        /* No `tcp6`: there is no IPv6 here, and a file that existed and was
         * empty would tell netstat the family is present with nothing on it. */
        static const char *const netfiles[] = {"dev", "route", "arp", "tcp"};
        int count = (int)(sizeof(netfiles) / sizeof(netfiles[0]));
        const char *entry = n < count ? netfiles[n] : synth_net_name(SYNTH_PROC_NETUDP + n - count);
        int i;

        if (!entry) {
            return 0;
        }
        for (i = 0; entry[i] != '\0'; i++) {
            name[i] = entry[i];
        }
        name[i] = '\0';
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
            *ino = procfs_ino_net(SYNTH_PROC_NETUDP + n - count);
        }
        return 1;
    }
    if (kind == VNODE_PROC_DIR) {
        static const struct {
            const char *name;
            uint32_t (*inode)(void);
            uint8_t type;
        } globals[] = {{"cmdline", procfs_ino_cmdline, DT_REG},
                       {"disk", procfs_ino_disk, DT_REG},
                       {"uptime", procfs_ino_uptime, DT_REG},
                       {"stat", procfs_ino_statsys, DT_REG},
                       {"meminfo", procfs_ino_meminfo, DT_REG},
                       {"cpuinfo", procfs_ino_cpuinfo, DT_REG},
                       {"loadavg", procfs_ino_loadavg, DT_REG},
                       {"diskstats", procfs_ino_diskstats, DT_REG},
                       {"version", procfs_ino_version, DT_REG},
                       {"partitions", procfs_ino_partitions, DT_REG},
                       {"mounts", procfs_ino_mounts, DT_REG},
                       {"net", procfs_ino_netdir, DT_DIR},
                       {"sys", procfs_ino_sysdir, DT_DIR},
                       {"self", procfs_ino_dir, DT_LNK},
                       {"usb", procfs_ino_usb, DT_REG},
                       {"loop", procfs_ino_loop, DT_REG},
                       {"console", procfs_ino_console, DT_REG},
                       {"audio", procfs_ino_audio, DT_REG},
                       {"interrupts", procfs_ino_interrupts, DT_REG},
                       {"filesystems", procfs_ino_filesystems, DT_REG}};
        int count = (int)(sizeof(globals) / sizeof(globals[0]));

        if (n < count) {
            int i;
            for (i = 0; globals[n].name[i] != '\0'; i++)
                name[i] = globals[n].name[i];
            name[i] = '\0';
            *dtype = globals[n].type;
            *ino = globals[n].inode();
            return 1;
        }
        {
            int p = procfs_nth_pid(n - count);
            if (p < 0) {
                return 0;
            }
            synth_u32_str(name, (uint32_t)p);
            *ino = procfs_ino_pid(p);
            *dtype = DT_DIR;
            return 1;
        }
    }
    if (kind == VNODE_PROC_PID || kind == VNODE_PROC_TASK_DIR || kind == VNODE_PROC_FD_DIR) {
        return synth_proc_dirent(sd, n, name, ino, dtype);
    }
    return 0;
}

/* getdents for a synthetic directory fd: pack records from synth_dirent_at,
 * advancing the fd's offset cursor (same record layout as the KRFFS path). */
static int32_t synth_getdents(fd_entry_t *e, char *buf, uint32_t count)
{
    static const struct synth_dir none = {VNODE_ENOENT, 0, 0, 0};
    const struct synth_dir *sd = e->priv ? (const struct synth_dir *)e->priv : &none;
    uint32_t written = 0;
    uint32_t idx = (uint32_t)e->ofd->offset;
    char name[32];
    uint64_t ino;
    uint8_t dtype;

    while (synth_dirent_at(sd, (int)idx, name, &ino, &dtype)) {
        union {
            struct linux_dirent64 rec;
            uint8_t bytes[64];
        } recbuf;
        uint32_t name_len = k_strlen(name);
        uint32_t rec_len =
            sizeof(uint64_t) + sizeof(int64_t) + sizeof(uint16_t) + sizeof(uint8_t) + name_len + 1;
        struct linux_dirent64 *d;

        rec_len = (rec_len + 7) & ~7u;
        if (written + rec_len > count) {
            if (written == 0) {
                return -EINVAL; /* buffer too small for even one entry */
            }
            break;
        }
        k_memset(recbuf.bytes, 0, rec_len);
        d = &recbuf.rec;
        d->d_ino = ino;
        d->d_off = (int64_t)(idx + 1);
        d->d_reclen = (uint16_t)rec_len;
        d->d_type = dtype;
        k_memcpy(d->d_name, name, name_len + 1);
        if (copy_to_user((void __user *)(buf + written), recbuf.bytes, rec_len) != 0) {
            return written > 0 ? (int32_t)written : -EFAULT;
        }
        written += rec_len;
        idx++;
        e->ofd->offset = idx;
    }
    return (int32_t)written;
}

/**
 * Read directory entries (getdents64(2)).
 *
 * EBX = fd, ECX = dirp buffer, EDX = count. Packs linux_dirent64 records
 * into the buffer, one per child, and advances the fd's offset cursor. A
 * record's `d_type` is the type the file system gives the child, DT_UNKNOWN
 * where it gives none, and never DT_REG for anything but a regular file.
 * Returns the bytes written, 0 at end of directory, or -errno.
 */
int32_t sys_getdents64(isr_params_t *params)
{
    int fd_num = (int)params->ebx;
    char *buf = (char *)params->ecx;
    uint32_t count = params->edx;
    fd_entry_t *entry;
    uint32_t bytes_written = 0;
    uint32_t name_len;
    uint32_t rec_len;
    struct linux_dirent64 *dirent;

    if (!current_proc || !buf || count == 0) {
        return -EFAULT;
    }

    /* Each record is built in a kernel buffer and copied out, and the whole
     * [buf, buf+count) span is range-checked and faulted in once here, so a
     * copy part way through the batch cannot meet an unmapped page. */
    if (!user_buffer_ok((uint32_t)buf, count, 1)) {
        return -EFAULT;
    }

    entry = fd_get(current_proc->fds, fd_num);
    if (!entry || (entry->flags & FD_FLAG_PATH)) {
        return -EBADF;
    }

    /* A synthetic directory (/dev, /proc, /proc/<pid>[/fd]) lists from the
     * process table or device registry, not KRFFS. Bare `ls /` does not merge
     * these mount names; they are reached by their absolute paths. */
    if (entry->type == FD_TYPE_SYNTH_DIR) {
        return synth_getdents(entry, buf, count);
    }

    if (entry->type != FD_TYPE_DIR) {
        return -ENOTDIR;
    }

    if (!vfs_root_mounted()) {
        return -EIO;
    }
    if (fd_node_check(entry) != 0) {
        return -EBADF; /* the directory was removed out from under this fd */
    }

    /*
     * The cursor is the type's own: for the KRFFS boot root the byte offset
     * of the last node returned plus one, for a mounted volume whatever its
     * readdir keeps. It is NOT a count of the children seen so far. That
     * distinction is the whole of a fault this had: `rm -rf` reads a batch
     * of names, unlinks them, and reads again, and a cursor that counted
     * children renumbered every surviving child when one was removed, so the
     * second read skipped as many entries as the first read had deleted. The
     * files left behind then made the directory refuse to be removed, with
     * `rm` reporting only that it was not empty
     * (tests/unlinkdirprobe.c).
     *
     * What an offset guarantees, exactly. Deleting an entry does not move any
     * other node: `krffs_remove_reserved_node` only widens free extents, and
     * allocation splits a free node instead of shifting a reserved one. So a
     * listing interleaved with deletions, which is the case that matters and
     * the one `rm` performs, sees every surviving entry once.
     *
     * What it does not guarantee. Growing a file CAN move its node: when the
     * following extent is not free and large enough,
     * `krffs_resize_reserved_node` allocates elsewhere, copies, and frees
     * the old node. A child that grows that way while another descriptor is
     * mid-listing lands at a new offset, and is missed if that offset is
     * below the cursor or repeated if above. The same is true of a file
     * created during a listing in space a removed one freed. Both are
     * concurrent modifications of a directory being read, which POSIX leaves
     * unspecified, and neither is reachable from the loop this exists for;
     * the previous count-based cursor failed the case that was reachable.
     */
    {
        struct open_file *ofd = entry->ofd;
        uint32_t cursor;

        if (!ofd || !ofd->vmnt) {
            return -EBADF;
        }
        fd_ofd_lock(ofd);
        cursor = (uint32_t)ofd->offset;
        for (;;) {
            struct vfs_dirent de;
            union {
                struct linux_dirent64 rec;
                uint8_t bytes[VFS_NAME_MAX + 1 + 32];
            } recbuf;
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
            name_len = k_strlen(de.name);
            /* Fixed fields (19 bytes) + name + NUL, padded to 8-byte
             * align. */
            rec_len = sizeof(uint64_t) + sizeof(int64_t) + sizeof(uint16_t) + sizeof(uint8_t) +
                      name_len + 1;
            rec_len = (rec_len + 7) & ~7;
            if (bytes_written + rec_len > count) {
                if (bytes_written == 0) {
                    fd_ofd_unlock(ofd);
                    return -EINVAL; /* buffer too small for even one entry */
                }
                break; /* the uncommitted cursor re-reads this entry next */
            }
            /* Build the record in kernel memory, then copy it out through
             * the helper (no raw write into user space). */
            k_memset(recbuf.bytes, 0, rec_len);
            dirent = &recbuf.rec;
            dirent->d_ino = de.ino;
            dirent->d_off = (int64_t)cursor; /* next position */
            dirent->d_reclen = (uint16_t)rec_len;
            dirent->d_type = (uint8_t)de.type;
            k_memcpy(dirent->d_name, de.name, name_len + 1);
            if (copy_to_user((void __user *)(buf + bytes_written), recbuf.bytes, rec_len) != 0) {
                fd_ofd_unlock(ofd);
                return (bytes_written > 0) ? (int32_t)bytes_written : -EFAULT;
            }
            bytes_written += rec_len;
            ofd->offset = cursor; /* committed with the record, not before */
        }
        fd_ofd_unlock(ofd);
    }

    /* Nothing written after scanning every node is the end of the directory. A
     * buffer too small for even one record returned -EINVAL above, so zero
     * here has only the one meaning. */
    return (int32_t)bytes_written;
}

/* dup3(330): like dup2 but oldfd == newfd is -EINVAL and a flag can set
 * O_CLOEXEC on the new descriptor. */
int32_t sys_dup3(isr_params_t *params)
{
    int oldfd = (int)params->ebx;
    int newfd = (int)params->ecx;
    uint32_t flags = params->edx;
    int32_t r;

    if (oldfd == newfd) {
        return -EINVAL;
    }
    if (flags & ~(uint32_t)O_CLOEXEC) {
        return -EINVAL;
    }
    r = sys_dup2(params); /* shares EBX=oldfd, ECX=newfd */
    if (r >= 0 && (flags & O_CLOEXEC)) {
        fd_entry_t *e = fd_get(current_proc->fds, newfd);
        if (e) {
            e->flags |= FD_FLAG_CLOEXEC;
        }
    }
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
 * mount and of /dev and /proc that `open_create` would give, as on Linux.
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
 * Make the socket file an AF_UNIX bind names. The file is made as O_CREAT
 * with O_EXCL makes one, with the socket type bits; the hold its creation
 * gave is kept for the socket's life, as a working directory keeps its own,
 * so the mount counts it and refuses to go while the socket is bound. A name
 * that exists, or one that names a directory by its end, makes nothing
 * (`bind_name_check`), as on Linux: -ENOENT after a slash for a name nothing
 * has, and -EADDRINUSE, which a name that exists answers, for one that does,
 * for a last "." or "..", and for the root, before a read-only mount's
 * -EROFS.
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
        rc = open_create(buf, O_EXCL, S_IFSOCK | (mode & 07777u), ref);
        if (rc == 0) {
            vfs_walk_unlock(ref->mnt);
            rc = vfs_getattr(ref->mnt, ref->node, &st);
            if (rc == 0) {
                *ino = (uint32_t)st.ino;
                ref->mnt->refs++;
                vfs_leave(ref->mnt);
            } else {
                vfs_ref_release(ref);
            }
        }
    }
    path_buf_put(buf);
    return (rc == 1 || rc == -EEXIST) ? -EADDRINUSE : rc;
}

void fs_socket_unbind(struct vfs_ref *ref)
{
    struct vfs_mount *mnt = ref->mnt;

    vfs_ref_put(ref);
    mnt->refs--;
}

/**
 * Find the socket file a connect or a send names: its mount and inode
 * number, after the caller's write permission on it is checked. A name that
 * names a directory by its end must reach one (`resolve_named`), so a socket
 * file so named is -ENOTDIR, as on Linux.
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
 * Read a file mapping's bytes: `count` bytes at `off` into the kernel buffer
 * `buf`, the description's offset untouched. It takes the mount's namespace
 * lock, which a type that moves a file's bytes needs, and not the
 * description's lock: a read into a mapping of the file being read faults
 * here while that lock is held.
 */
int32_t fs_write_at(fd_entry_t *e, const void *buf, uint32_t count, uint64_t off)
{
    struct open_file *ofd = e->ofd;
    int32_t n;

    if (e->type != FD_TYPE_FILE || !ofd || !ofd->vmnt) {
        return -ENODEV;
    }
    if (ofd->vmnt->type->ns_serialised) {
        vfs_ns_lock(ofd->vmnt);
    }
    n = vfs_write(ofd->vmnt, ofd->fhandle, buf, count, off);
    if (ofd->vmnt->type->ns_serialised) {
        vfs_ns_unlock(ofd->vmnt);
    }
    return n;
}

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
