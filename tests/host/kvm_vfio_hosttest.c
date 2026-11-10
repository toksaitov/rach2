/* Check the KVM VFIO bridge's descriptor ownership and DMA cache policy.
 * host-cflags: -DRACH2_HOST_X86_64 -DRACH2_HOST_CPU
 */
#include "kvm/vfio.c"

extern int printf(const char *format, ...);
extern void *malloc(unsigned long n);
extern void free(void *p);

static int failures, live, fail_alloc, fail_fd, fail_out, caller_ok = 1;
static int releases, coherency_on, coherency_off, waits;
static fd_entry_t entries[16];
static struct open_file descriptions[16];
static fd_table_t table = {entries, 16, 1, 0, NULL};
static process_t caller;
struct percpu cpu_table[NR_CPUS] = {{.current = &caller}};
unsigned int host_cpu;

/** Record an assertion, keeping later cleanup checks running. */
#define CHECK(c)                                                    \
    do {                                                            \
        if (!(c)) {                                                 \
            printf("kvm-vfio-hosttest: fail: line %d\n", __LINE__); \
            failures++;                                             \
        }                                                           \
    } while (0)

/** Zero memory without a host library's kernel type declarations. */
static void zero(void *p, size_t n)
{
    unsigned char *b = p;
    while (n--) {
        *b++ = 0;
    }
}

/** Count each bridge allocation and inject one allocation failure. */
void *kzalloc(size_t n)
{
    void *p;
    if (fail_alloc) {
        fail_alloc = 0;
        return NULL;
    }
    p = malloc(n);
    if (p) {
        live++;
        zero(p, n);
    }
    return p;
}

/** Return a counted allocation. */
void kfree(void *p)
{
    if (p) {
        live--;
        free(p);
    }
}

/** Copy valid host pointers, treating low addresses as user faults. */
int copy_from_user(void *d, const void __user *s, size_t n)
{
    unsigned char *out = d;
    const unsigned char *in = s;
    if ((uintptr_t)s < 4096) {
        return -1;
    }
    while (n--) {
        *out++ = *in++;
    }
    return 0;
}

/** Inject the copy-back failure after the bridge has been allocated. */
int copy_to_user(void __user *d, const void *s, size_t n)
{
    if (fail_out || (uintptr_t)d < 4096) {
        return -1;
    }
    return copy_from_user(d, s, n);
}

/** Reserve a table slot and its open file description. */
int fd_alloc(fd_table_t *t)
{
    int i;
    if (fail_fd) {
        return -EMFILE;
    }
    for (i = 0; i < t->cap; i++) {
        if (!t->entries[i].ofd) {
            zero(&descriptions[i], sizeof(descriptions[i]));
            descriptions[i].refcount = 1;
            t->entries[i].ofd = &descriptions[i];
            return i;
        }
    }
    return -EMFILE;
}

/** Look up a published descriptor. */
fd_entry_t *fd_get(fd_table_t *t, int fd)
{
    return fd >= 0 && fd < t->cap && t->entries[fd].type != FD_TYPE_NONE ? &t->entries[fd] : NULL;
}

/** Hold one file description. */
void fd_ofd_ref(struct open_file *f)
{
    f->refcount++;
}

/** Drop one file description. */
void fd_ofd_unref(struct open_file *f)
{
    CHECK(f->refcount > 0);
    f->refcount--;
}

/** Release the slot after the caller has released its backing object. */
int fd_free(fd_table_t *t, int fd)
{
    fd_ofd_unref(t->entries[fd].ofd);
    zero(&t->entries[fd], sizeof(t->entries[fd]));
    return 0;
}

/** Call close once, at the final reference, as syscall_fs.c does. */
void fd_release_backing(fd_entry_t *e)
{
    if (e->ofd->refcount == 1 && e->ops && e->ops->close) {
        e->ops->close(e);
    }
}

/** Model the VM's address-space check. */
int kvm_vm_caller_ok(struct kvm_vm *vm)
{
    (void)vm;
    return caller_ok;
}

/** Hold the VM independently of its own descriptor. */
void kvm_vm_get(struct kvm_vm *vm)
{
    vm->refs++;
}

/** Drop the bridge's VM reference. */
void kvm_vm_put(struct kvm_vm *vm)
{
    CHECK(vm->refs > 0);
    vm->refs--;
}

/** Count each transition to or from the guest PAT's memory types. */
void kvm_tdp_noncoherent(struct kvm_vm *vm, int assigned)
{
    if (assigned) {
        coherency_on++;
        vm->noncoherent++;
    } else {
        CHECK(vm->noncoherent == 1);
        coherency_off++;
        vm->noncoherent--;
    }
}

/** A group and a device are valid; an ordinary anonymous file is not. */
int vfio_file_is_valid(const struct fd_entry *e)
{
    return e->priv == (void *)1 || e->priv == (void *)2;
}

/** Finish the preceding operation when a caller must wait for it. */
int wq_wait_event_nointr(wait_queue_t *wq, wq_cond_fn cond, void *arg)
{
    (void)wq;
    waits++;
    CHECK(!cond(arg));
    ((struct bridge *)arg)->busy = 0;
    return 0;
}

/** The wake has no sleeping thread in this model. */
void wq_wake(wait_queue_t *wq, uint32_t kinds)
{
    (void)wq;
    CHECK(kinds == WQ_DATA);
}

/** Count a VFIO backing object's final release. */
static void source_close(fd_entry_t *e)
{
    CHECK(e->ofd->refcount == 1);
    releases++;
}

static const struct file_ops source_ops = {NULL, NULL, NULL, NULL, source_close, NULL, NULL};

/** Open one modeled source file. */
static int source_open(int kind)
{
    int fd = fd_alloc(&table);
    entries[fd].type = kind == 1 ? FD_TYPE_DEVICE : FD_TYPE_ANON;
    entries[fd].priv = (void *)(uintptr_t)kind;
    entries[fd].ops = &source_ops;
    return fd;
}

/** Close the descriptor without touching a file still held by the bridge. */
static void close_fd(int fd)
{
    fd_release_backing(&entries[fd]);
    fd_free(&table, fd);
}

/** Set one attribute with Linux's pointer-to-fd payload. */
static int attr_set(int bridge_fd, int fd, uint64_t attr)
{
    struct kvm_device_attr a = {0, KVM_DEV_VFIO_FILE, 0, 0};
    a.attr = attr;
    a.addr = (uintptr_t)&fd;
    return bridge_ioctl(&entries[bridge_fd], KVM_SET_DEVICE_ATTR, (uintptr_t)&a);
}

/** Check creation rollback, the one-bridge rule and attribute errors. */
static int create_rows(struct kvm_vm *vm)
{
    struct kvm_create_device d = {KVM_DEV_TYPE_VFIO, 99, KVM_CREATE_DEVICE_TEST};
    struct kvm_device_attr a = {0, KVM_DEV_VFIO_FILE, KVM_DEV_VFIO_FILE_ADD, 0};
    int b;
    CHECK(sizeof(d) == 12 && sizeof(a) == 24);
    CHECK(KVM_CREATE_DEVICE == 0xc00caee0u && KVM_SET_DEVICE_ATTR == 0x4018aee1u);
    CHECK(KVM_GET_DEVICE_ATTR == 0x4018aee2u && KVM_HAS_DEVICE_ATTR == 0x4018aee3u);
    CHECK(kvm_vfio_create(vm, 0) == -EFAULT);
    CHECK(kvm_vfio_create(vm, (uintptr_t)&d) == 0 && d.fd == 99 && !vm->vfio);
    d.type = 123;
    CHECK(kvm_vfio_create(vm, (uintptr_t)&d) == -ENODEV);
    d.type = KVM_DEV_TYPE_VFIO;
    d.flags = 0x80000000u;
    fail_alloc = 1;
    CHECK(kvm_vfio_create(vm, (uintptr_t)&d) == -ENOMEM);
    fail_fd = 1;
    CHECK(kvm_vfio_create(vm, (uintptr_t)&d) == -EMFILE && live == 0);
    fail_fd = 0;
    fail_out = 1;
    CHECK(kvm_vfio_create(vm, (uintptr_t)&d) == -EFAULT && live == 0 && vm->refs == 1 && !vm->vfio);
    fail_out = 0;
    CHECK(kvm_vfio_create(vm, (uintptr_t)&d) == 0);
    b = (int)d.fd;
    CHECK(vm->refs == 2 && live == 1 &&
          entries[b].flags == (FD_FLAG_READ | FD_FLAG_WRITE | FD_FLAG_CLOEXEC));
    CHECK(kvm_vfio_create(vm, (uintptr_t)&d) == -EBUSY);
    d.flags = KVM_CREATE_DEVICE_TEST;
    CHECK(kvm_vfio_create(vm, (uintptr_t)&d) == 0);
    CHECK(bridge_ioctl(&entries[b], KVM_GET_DEVICE_ATTR, 0) == -EPERM);
    CHECK(bridge_ioctl(&entries[b], 0, 0) == -ENOTTY);
    CHECK(bridge_ioctl(&entries[b], KVM_SET_DEVICE_ATTR, 0) == -EFAULT);
    CHECK(bridge_ioctl(&entries[b], KVM_SET_DEVICE_ATTR, (uintptr_t)&a) == -EFAULT);
    a.flags = 0xffffffffu;
    CHECK(bridge_ioctl(&entries[b], KVM_HAS_DEVICE_ATTR, (uintptr_t)&a) == 0);
    a.group = 2;
    CHECK(bridge_ioctl(&entries[b], KVM_HAS_DEVICE_ATTR, (uintptr_t)&a) == -ENXIO);
    a.group = 1;
    a.attr = 3;
    CHECK(bridge_ioctl(&entries[b], KVM_SET_DEVICE_ATTR, (uintptr_t)&a) == -ENXIO);
    caller_ok = 0;
    CHECK(bridge_ioctl(&entries[b], KVM_HAS_DEVICE_ATTR, (uintptr_t)&a) == -EIO);
    caller_ok = 1;
    return b;
}

/** Check aliases, file ownership and DMA cache policy through final close. */
int main(void)
{
    struct kvm_vm vm;
    int b, group, device, other, alias;

    zero(&vm, sizeof(vm));
    vm.refs = 1;
    caller.fds = &table;
    b = create_rows(&vm);
    group = source_open(1);
    device = source_open(2);
    other = source_open(3);
    CHECK(attr_set(b, -1, KVM_DEV_VFIO_FILE_ADD) == -EBADF);
    CHECK(attr_set(b, other, KVM_DEV_VFIO_FILE_ADD) == -EINVAL);
    CHECK(attr_set(b, other, KVM_DEV_VFIO_FILE_DEL) == -ENOENT);
    fail_alloc = 1;
    CHECK(attr_set(b, group, KVM_DEV_VFIO_FILE_ADD) == -ENOMEM &&
          descriptions[group].refcount == 1);
    ((struct bridge *)vm.vfio)->busy = 1;
    CHECK(attr_set(b, group, KVM_DEV_VFIO_FILE_ADD) == 0 && waits == 1);
    CHECK(descriptions[group].refcount == 2 && coherency_on == 1 && vm.noncoherent == 1);
    alias = fd_alloc(&table);
    fd_ofd_unref(entries[alias].ofd);
    entries[alias] = entries[group];
    fd_ofd_ref(entries[alias].ofd);
    CHECK(attr_set(b, alias, KVM_DEV_VFIO_FILE_ADD) == -EEXIST);
    CHECK(attr_set(b, device, KVM_DEV_VFIO_FILE_ADD) == 0 && coherency_on == 1);
    close_fd(group);
    CHECK(releases == 0);
    CHECK(attr_set(b, alias, KVM_DEV_VFIO_FILE_DEL) == 0 && coherency_off == 0);
    close_fd(alias);
    CHECK(releases == 1);
    CHECK(attr_set(b, device, KVM_DEV_VFIO_FILE_DEL) == 0 && coherency_off == 1);
    CHECK(attr_set(b, device, KVM_DEV_VFIO_FILE_ADD) == 0 && coherency_on == 2);
    close_fd(device);
    CHECK(releases == 1);
    kvm_vm_put(&vm);
    CHECK(vm.refs == 1);
    close_fd(b);
    CHECK(releases == 2 && vm.refs == 0 && !vm.vfio && !vm.noncoherent && coherency_off == 2 &&
          live == 0);
    close_fd(other);
    CHECK(releases == 3);
    printf("kvm-vfio-hosttest: %s\n", failures ? "fail" : "pass");
    return failures != 0;
}
