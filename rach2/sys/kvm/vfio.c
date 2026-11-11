/* The KVM VFIO bridge holds assigned files and their DMA cache policy. */
#include "kvm.h"

#include "../fs/fd.h"
#include "../fs/file.h"
#include "../fs/krffs.h"
#include "../lib/kerrno.h"
#include "../mm/kheap.h"
#include "../mm/usercopy.h"
#include "../proc/process.h"
#include "../syscall_internal.h"
#include "../vfio/vfio.h"

struct bridge_file {
    fd_entry_t entry; /* holds its open file description until removal */
    struct bridge_file *next;
};

struct bridge {
    struct anon_obj anon;
    struct kvm_vm *vm;
    struct bridge_file *files;
    wait_queue_t wq;
    int busy; /* serializes changes across a file release that sleeps */
};

/** Whether the bridge has no attribute operation in progress. */
static int bridge_idle(void *arg)
{
    return !((struct bridge *)arg)->busy;
}

/** Drop the saved file reference; its last close can sleep. */
static void bridge_file_put(struct bridge_file *f)
{
    fd_release_backing(&f->entry);
    kfree(f);
}

/** Add or remove `fd`, with the bridge held. VFIO does not enforce cache
 * coherence, so any member requires guest PAT memory types in KVM. */
static int32_t bridge_file_set(struct bridge *b, uint64_t attr, int fd)
{
    fd_entry_t *e = fd_get(current_proc->fds, fd);
    struct bridge_file *f, **at;

    if (!e) {
        return -EBADF;
    }
    if (attr == KVM_DEV_VFIO_FILE_ADD && !vfio_file_is_valid(e)) {
        return -EINVAL;
    }
    for (at = &b->files; *at && (*at)->entry.ofd != e->ofd; at = &(*at)->next) {
    }
    if (attr == KVM_DEV_VFIO_FILE_DEL) {
        f = *at;
        if (!f) {
            return -ENOENT;
        }
        *at = f->next;
        bridge_file_put(f);
        if (!b->files) {
            kvm_tdp_noncoherent(b->vm, 0);
        }
        return 0;
    }
    if (*at) {
        return -EEXIST;
    }
    f = kzalloc(sizeof(*f));
    if (!f) {
        return -ENOMEM;
    }
    f->entry = *e;
    fd_ofd_ref(e->ofd);
    if (!b->files) {
        kvm_tdp_noncoherent(b->vm, 1);
    }
    *at = f;
    return 0;
}

/** The bridge's device attributes, with Linux's errors and caller check. */
static int32_t bridge_ioctl(fd_entry_t *e, uint32_t request, uintptr_t arg)
{
    struct bridge *b = (struct bridge *)e->priv;
    struct kvm_device_attr a;
    int32_t fd, r;

    if (!kvm_vm_caller_ok(b->vm)) {
        return -EIO;
    }
    if (request == KVM_GET_DEVICE_ATTR) {
        return -EPERM;
    }
    if (request != KVM_SET_DEVICE_ATTR && request != KVM_HAS_DEVICE_ATTR) {
        return -ENOTTY;
    }
    if (copy_from_user(&a, (const void __user *)arg, sizeof(a))) {
        return -EFAULT;
    }
    if (a.group != KVM_DEV_VFIO_FILE ||
        (a.attr != KVM_DEV_VFIO_FILE_ADD && a.attr != KVM_DEV_VFIO_FILE_DEL)) {
        return -ENXIO;
    }
    if (request == KVM_HAS_DEVICE_ATTR) {
        return 0;
    }
    if (copy_from_user(&fd, (const void __user *)(uintptr_t)a.addr, sizeof(fd))) {
        return -EFAULT;
    }
    while (b->busy) {
        wq_wait_event_nointr(&b->wq, bridge_idle, b);
    }
    b->busy = 1;
    r = bridge_file_set(b, a.attr, fd);
    b->busy = 0;
    wq_wake(&b->wq, WQ_DATA);
    return r;
}

/** Release the bridge and each held file before dropping its VM reference. */
static void bridge_close(fd_entry_t *e)
{
    struct bridge *b = (struct bridge *)e->priv;
    struct kvm_vm *vm = b->vm;
    struct bridge_file *f;
    int assigned = b->files != NULL;

    while ((f = b->files) != NULL) {
        b->files = f->next;
        bridge_file_put(f);
    }
    if (assigned) {
        kvm_tdp_noncoherent(vm, 0);
    }
    vm->vfio = NULL;
    e->priv = NULL;
    kfree(b);
    kvm_vm_put(vm);
}

static const struct file_ops bridge_ops = {NULL,         NULL, NULL, bridge_ioctl,
                                           bridge_close, NULL, NULL, 0};

int32_t kvm_vfio_create(struct kvm_vm *vm, uintptr_t arg)
{
    struct kvm_create_device d;
    struct bridge *b;
    fd_entry_t *e, cleanup;
    int fd;

    if (copy_from_user(&d, (const void __user *)arg, sizeof(d))) {
        return -EFAULT;
    }
    if (d.type != KVM_DEV_TYPE_VFIO) {
        return -ENODEV;
    }
    if (d.flags & KVM_CREATE_DEVICE_TEST) {
        return copy_to_user((void __user *)arg, &d, sizeof(d)) ? -EFAULT : 0;
    }
    if (vm->vfio) {
        return -EBUSY;
    }
    b = kzalloc(sizeof(*b));
    if (!b) {
        return -ENOMEM;
    }
    fd = fd_alloc(current_proc->fds);
    if (fd < 0) {
        kfree(b);
        return fd;
    }
    b->anon.name = "kvm-vfio";
    b->vm = vm;
    vm->vfio = b;
    kvm_vm_get(vm);
    d.fd = (uint32_t)fd;
    if (copy_to_user((void __user *)arg, &d, sizeof(d))) {
        cleanup.priv = b;
        bridge_close(&cleanup);
        fd_free(current_proc->fds, fd);
        return -EFAULT;
    }
    e = fd_slot(current_proc->fds, fd);
    e->type = FD_TYPE_ANON;
    e->ops = &bridge_ops;
    e->priv = b;
    e->flags = FD_FLAG_READ | FD_FLAG_WRITE | FD_FLAG_CLOEXEC;
    fd_install(current_proc->fds, fd);
    return 0;
}
