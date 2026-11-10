/**
 * Fuzz harness for the ioctl decoders of the display stack: the DRM core and
 * KMS over vkms, PRIME and sync objects over a render device of the
 * harness's, dma-bufs, sync files, sw_sync's timelines and evdev. The input
 * is a program of opens, ioctls, reads, polls, closes, input events, CRC
 * reads and clock steps, run as one process.
 *
 * fuzz-deps: rach2/sys/drm/drm.c rach2/sys/drm/drm_auth.c
 * fuzz-deps: rach2/sys/drm/drm_gem.c rach2/sys/drm/drm_mode.c
 * fuzz-deps: rach2/sys/drm/drm_atomic.c rach2/sys/drm/drm_vblank.c
 * fuzz-deps: rach2/sys/drm/drm_legacy.c rach2/sys/drm/drm_edid.c
 * fuzz-deps: rach2/sys/drm/drm_prime.c rach2/sys/drm/drm_syncobj.c
 * fuzz-deps: rach2/sys/dmabuf/dmabuf.c rach2/sys/dmabuf/fence.c
 * fuzz-deps: rach2/sys/dmabuf/sync_file.c rach2/sys/dmabuf/sw_sync.c
 * fuzz-deps: rach2/sys/drm/drm_debugfs.c rach2/sys/fs/debugfs.c
 * fuzz-deps: rach2/sys/arch/x86_64/ia32_ioctl_drm.c
 * fuzz-deps: rach2/sys/arch/x86_64/ia32_ioctl_gpu.c
 * fuzz-deps: rach2/sys/evdev.c
 * fuzz-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * fuzz-deps: tests/host/compat/spinlock_host.c
 *
 * The kernel's own sources are linked whole, each its own unit, and vkms
 * is included here, so that the harness reaches its composer and clock.
 * Around them is a model of the rest of the kernel: one process with a
 * table of descriptors, a clock that moves only when the program says or a
 * wait would sleep, and user memory that is an arena of 64 KiB. A user
 * address is an offset into the arena; one below 4096, or a range that
 * leaves the arena, faults, so no input reaches the host's memory through
 * a pointer it chose. A wait that would sleep moves the clock, runs the
 * vblanks, the composer and the commit queue, and then, when its condition
 * still does not hold, returns as a signal would; a wait that takes no
 * signal and never ends is reported and aborts, since it would hang the
 * caller for good.
 *
 * The input: a byte of flags (bit 0 runs the program as root, bit 1 gives
 * vkms no cursor plane when the harness first starts, bit 2 makes the
 * program an i386 one on the x86-64 kernel), a 16-bit length and
 * that many bytes put in the arena at DATA_OFF, then the steps, each an
 * operation byte and its operands, until the input ends. An open names
 * vkms's card0, the render device's card1 or renderD129, a sw_sync
 * timeline, or the mouse's or the touchpad's event node, or a file of
 * debugfs by its path, the DRM devices' and sync's. An ioctl's
 * command is built from a type of the stack's (`d`, `E`, `>`, `b`, `W`,
 * or 32 bits as given), a number, a direction and a size, so that every
 * number of every decoder is a byte away; its argument is the bytes that
 * follow, at ARG_OFF of the arena. An i386 program's ioctls of the DRM
 * core and of dma-buf, sync files and udmabuf go through the compat
 * path's translations, ia32_ioctl_drm and ia32_ioctl_gpu, which run the
 * native request on a structure in kernel memory that a user-copy window
 * opens, and its reads of an event node take the 16-byte records; its
 * other ioctls pass to the native path, as the compat table passes them.
 * A write gives a descriptor the bytes that follow. After the steps every
 * descriptor is
 * closed, and the devices keep what the closes leave, as on a machine
 * whose programs have ended. The 16-bit and 32-bit operands are
 * little-endian. Replayed, the harness prints each ioctl and its result.
 */
#include "drm/vkms.c"
#include "arch/x86_64/ia32.h"
#include "dmabuf/dmabuf.h"
#include "evdev.h"
#include "fs/debugfs.h"
#include "fs/devfs.h"
#include "fs/fd.h"
#include "fs/vfs.h"
#include "fs/file.h"
#include "io/eventfd.h"
#include "input.h"
#include "io/poll.h"
#include "mm/mm.h"
#include "mouse.h"
#include "mm/usercopy.h"
#include "proc/percpu.h"
#include "proc/tgroup.h"
#include "syscall.h"
#include "timer.h"
#include "../fuzz/fuzz.h"

extern void *calloc(unsigned long count, unsigned long size);
extern void free(void *p);
extern void *memcpy(void *dst, const void *src, unsigned long n);
extern void *memset(void *dst, int c, unsigned long n);
extern int strcmp(const char *a, const char *b);
extern int fprintf(void *stream, const char *format, ...);
#ifdef __APPLE__
extern void *__stderrp; /* macOS's C library names its stderr so */
#define stderr __stderrp
#else
extern void *stderr;
#endif
extern void abort(void);

#ifdef FUZZ_REPLAY
#define FUZZ_LOG 1
#else
#define FUZZ_LOG 0
#endif

/* ---- the user's memory ---- */

#define ARENA    65536u
#define ARG_OFF  4096u /* an ioctl's argument */
#define ARG_MAX  4096u
#define READ_OFF 8192u /* a read's buffer */
#define READ_MAX 8192u
#define DATA_OFF 16384u /* the input's own bytes */

static uint8_t arena[ARENA];

/* The user-copy window of the compat path: kernel memory that the copies
 * take as the user's while it is open, or none. */
static uint8_t *window;
static size_t window_len;

void user_window_open(void *buf, uint32_t len)
{
    if (window) {
        fprintf(stderr, "drm-ioctl-fuzz: a window opened inside another\n");
        abort();
    }
    window = buf;
    window_len = len;
}

void user_window_close(void)
{
    window = NULL;
    window_len = 0;
}

/** Whether [`u`, `u` + `n`) lies wholly in the open window. */
static int in_window(uintptr_t u, size_t n)
{
    return window && u >= (uintptr_t)window && n <= window_len &&
           u - (uintptr_t)window <= window_len - n;
}

/** The host address of the user range [`u`, `u` + `n`): in the open
 * window, or in the arena above its first page; NULL for any other. */
static uint8_t *user_at(uintptr_t u, size_t n)
{
    if (in_window(u, n)) {
        return (uint8_t *)u;
    }
    return u >= 4096 && u <= ARENA && n <= ARENA - u ? arena + u : NULL;
}

int user_range_ok(uintptr_t addr, size_t len)
{
    return len == 0 || user_at(addr, len) != NULL;
}

int copy_from_user(void *dst, const void __user *src, size_t n)
{
    uint8_t *at = user_at((uintptr_t)src, n);

    if (!at) {
        return -1;
    }
    memcpy(dst, at, n);
    return 0;
}

int copy_to_user(void __user *dst, const void *src, size_t n)
{
    uint8_t *at = user_at((uintptr_t)dst, n);

    if (!at) {
        return -1;
    }
    memcpy(at, src, n);
    return 0;
}

/* ---- the kernel around the stack ---- */

struct percpu cpu_table[NR_CPUS];

/** No log, unless replaying. */
void klog(int level, const char *subsystem, const char *format, ...)
{
    (void)level;
    if (FUZZ_LOG && level <= KLOG_WARN) {
        fprintf(stderr, "klog: %s: %s\n", subsystem, format);
    }
}

/* The boot line: vkms on, with its overlays. */
static const char *cmdline = "rach2.vkms vkms.enable_overlay=1";
static int no_cursor;

int kcmdline_has(const char *word)
{
    return strcmp(word, "rach2.vkms") == 0;
}

int kcmdline_value(const char *key, char *out, unsigned int outlen)
{
    (void)cmdline;
    if (outlen < 2) {
        return 0;
    }
    if (strcmp(key, "vkms.enable_overlay") == 0 ||
        (no_cursor && strcmp(key, "vkms.enable_cursor") == 0)) {
        out[0] = strcmp(key, "vkms.enable_overlay") == 0 ? '1' : '0';
        out[1] = '\0';
        return 1;
    }
    return 0;
}

static int irq_depth;

uint32_t kirq_save(void)
{
    irq_depth++;
    return 0;
}

void kirq_restore(uint32_t flags)
{
    (void)flags;
    if (irq_depth <= 0) {
        fprintf(stderr, "drm-ioctl-fuzz: interrupts restored more often than saved\n");
        abort();
    }
    irq_depth--;
}

/* The largest allocation the model's heap grants: the kernel's heap is
 * 256 MiB on i686 and about 1 GiB in an x86-64 guest of 8 GiB, and a
 * request it has no room for answers NULL, as here one above this does. */
#define HEAP_LARGEST (64ul << 20)

void *kmalloc(size_t size)
{
    return size > HEAP_LARGEST ? NULL : calloc(1, size ? size : 1);
}

void *kzalloc(size_t size)
{
    return kmalloc(size);
}

void kfree(void *p)
{
    free(p);
}

/* The model's clock, in nanoseconds since boot. */
static int64_t now_ns = 1000000000ll;

void rtc_monotonic(int64_t *sec, int64_t *nsec)
{
    *sec = now_ns / 1000000000ll;
    *nsec = now_ns % 1000000000ll;
}

void rtc_realtime(int64_t *sec, int64_t *nsec)
{
    *sec = 1700000000ll + now_ns / 1000000000ll;
    *nsec = now_ns % 1000000000ll;
}

/** The wall clock, for debugfs's file times. */
void get_current_time(int64_t *seconds, int64_t *nanoseconds)
{
    rtc_realtime(seconds, nanoseconds);
}

uint64_t rtc_ticks_now(void)
{
    return (uint64_t)now_ns / (1000000000ull / TICK_HZ);
}

uint64_t tick_div(uint64_t n, uint32_t d, uint32_t *rem)
{
    if (rem) {
        *rem = (uint32_t)(n % d);
    }
    return n / d;
}

void wait_queue_init(wait_queue_t *wq)
{
    (void)wq;
}

void wq_wake(wait_queue_t *wq, uint32_t kinds)
{
    (void)wq;
    (void)kinds;
}

void wq_wake_all(wait_queue_t *wq)
{
    (void)wq;
}

static void run_time(int frames);

/** A wait: the clock runs, a frame at a time, up to a second, while the
 * condition does not hold; then the signal that ends an interruptible
 * wait. */
int wq_wait_event(wait_queue_t *wq, wq_cond_fn cond, void *arg)
{
    int i;

    (void)wq;
    for (i = 0; i < 60 && !cond(arg); i++) {
        run_time(1);
    }
    return cond(arg) ? 0 : -EINTR;
}

/** A wait no signal ends: the clock runs for a minute at most, and a wait
 * that still has not ended would never end. */
int wq_wait_event_nointr(wait_queue_t *wq, wq_cond_fn cond, void *arg)
{
    int i;

    (void)wq;
    for (i = 0; i < 3600 && !cond(arg); i++) {
        run_time(1);
    }
    if (!cond(arg)) {
        fprintf(stderr, "drm-ioctl-fuzz: an uninterruptible wait never ends\n");
        abort();
    }
    return 0;
}

/* ---- devices ---- */

#define NODES 8
static struct devfs_node *nodes[NODES];

int32_t devfs_node_register(struct devfs_node *node)
{
    int i;

    for (i = 0; i < NODES; i++) {
        if (!nodes[i]) {
            nodes[i] = node;
            return 0;
        }
    }
    return -ENOSPC;
}

void devfs_node_unregister(struct devfs_node *node)
{
    int i;

    for (i = 0; i < NODES; i++) {
        if (nodes[i] == node) {
            nodes[i] = NULL;
        }
    }
}

/** Return the registered node called NAME, or NULL. */
static struct devfs_node *node_named(const char *name)
{
    int i;

    for (i = 0; i < NODES; i++) {
        if (nodes[i] && strcmp(nodes[i]->name, name) == 0) {
            return nodes[i];
        }
    }
    return NULL;
}

int32_t drm_sysfs_register(struct drm_device *dev)
{
    (void)dev;
    return 0;
}

void drm_sysfs_unregister(struct drm_device *dev)
{
    (void)dev;
}

void drm_client_dev_register(struct drm_device *dev)
{
    (void)dev;
}

void drm_client_dev_unregister(struct drm_device *dev)
{
    (void)dev;
}

void drm_client_dev_hotplug(struct drm_device *dev)
{
    (void)dev;
}

void drm_sysfs_connector_hotplug_event(struct drm_connector *connector)
{
    (void)connector;
}

void arch_drm_probe(int phase)
{
    (void)phase;
}

int32_t virtio_gpu_probe(void)
{
    return -ENODEV;
}

int32_t vgem_probe(void)
{
    return -ENODEV;
}

/** No region is shared with a VM here. */
int32_t vgpu_shm_start(void)
{
    return 0;
}

int32_t udmabuf_start(void)
{
    return 0;
}

void paging_zap_device(const void *obj, uint32_t pgoff, uint32_t count)
{
    (void)obj;
    (void)pgoff;
    (void)count;
}

/** No mapping is made: the program has no mmap. */
int vm_device_map(vma_t *vma, const struct vm_device_ops *ops, void *obj, uint32_t cache)
{
    (void)vma;
    (void)ops;
    (void)obj;
    (void)cache;
    return -EINVAL;
}

/* The shmem helper's frames: pages of the host's, up to 4 MiB an object. */
#define MODEL_PAGES 1024

uintptr_t shmem_resident(const struct shmem *sh, uint32_t pgoff)
{
    if (!sh->leaves || pgoff >= MODEL_PAGES) {
        return 0;
    }
    return sh->leaves[0][pgoff];
}

int shmem_allocate(struct shmem *sh, uint64_t off, uint64_t len)
{
    uint64_t p;

    if (off + len < off || off + len > (uint64_t)MODEL_PAGES * PAGE_SIZE) {
        return -ENOSPC;
    }
    if (!sh->leaves) {
        sh->leaves = (uintptr_t **)calloc(1, sizeof(uintptr_t *));
        sh->leaves[0] = (uintptr_t *)calloc(MODEL_PAGES, sizeof(uintptr_t));
        sh->nleaves = 1;
    }
    for (p = off / PAGE_SIZE; p < (off + len + PAGE_SIZE - 1) / PAGE_SIZE; p++) {
        if (!sh->leaves[0][p]) {
            sh->leaves[0][p] = (uintptr_t)calloc(1, PAGE_SIZE);
            sh->held++;
        }
    }
    return 0;
}

void shmem_free(struct shmem *sh)
{
    uint32_t p;

    if (!sh->leaves) {
        return;
    }
    for (p = 0; p < MODEL_PAGES; p++) {
        free((void *)sh->leaves[0][p]);
    }
    free(sh->leaves[0]);
    free(sh->leaves);
    sh->leaves = NULL;
    sh->held = 0;
}

/** A frame's mapping is its address: the frames are the host's pages. */
void *kmap_temp(uintptr_t phys)
{
    return (void *)phys;
}

void kunmap_temp(void *va)
{
    (void)va;
}

/** No eventfd: the program makes none. */
struct eventfd *eventfd_get(int fd, int32_t *err)
{
    (void)fd;
    *err = -EBADF;
    return NULL;
}

void eventfd_put(struct eventfd *efd)
{
    (void)efd;
}

void eventfd_signal(struct eventfd *efd)
{
    (void)efd;
}

static int repeat_delay = 250, repeat_period = 33;

void input_get_repeat(int *delay_ms, int *period_ms)
{
    *delay_ms = repeat_delay;
    *period_ms = repeat_period;
}

/** No keyboard here takes its keys through input.c. */
int input_keyboard_release(struct evdev *dev, unsigned int keycode)
{
    (void)dev;
    (void)keycode;
    return 0;
}

void input_set_repeat(int delay_ms, int period_ms)
{
    repeat_delay = delay_ms;
    repeat_period = period_ms;
}

/** The harness has no console, so a key of a device goes nowhere. */
void input_console_key(const void *device, unsigned int keycode, int value)
{
    (void)device;
    (void)keycode;
    (void)value;
}

/**
 * Take an event a program wrote to a keyboard's node, or an LED or repeat
 * event to any node, which the kernel gives the console; the harness has
 * none, so the event goes nowhere.
 */
void input_written(uint16_t type, uint16_t code, int32_t value)
{
    (void)type;
    (void)code;
    (void)value;
}

/**
 * Take a mouse's event a program wrote to its node, which the kernel gives
 * the mouse queue; the harness has none, so the event goes nowhere.
 */
void mouse_written(uint16_t type, uint16_t code, int32_t value)
{
    (void)type;
    (void)code;
    (void)value;
}

/* ---- the process and its descriptors ---- */

#define FDS 32
static fd_entry_t fd_entries[FDS];
static fd_table_t fd_table = {fd_entries, FDS, 1, 0, NULL};
static process_t self;
static struct tgroup self_tg;
static struct mm self_mm;

int fd_alloc(fd_table_t *table)
{
    int i;

    for (i = 0; i < table->cap; i++) {
        if (table->entries[i].type == FD_TYPE_NONE) {
            memset(&table->entries[i], 0, sizeof(table->entries[i]));
            table->entries[i].type = FD_TYPE_ANON;
            return i;
        }
    }
    return -EMFILE;
}

int fd_free(fd_table_t *table, int fd)
{
    if (fd < 0 || fd >= table->cap) {
        return -EBADF;
    }
    memset(&table->entries[fd], 0, sizeof(table->entries[fd]));
    return 0;
}

fd_entry_t *fd_get(fd_table_t *table, int fd)
{
    if (!table || fd < 0 || fd >= table->cap || table->entries[fd].type == FD_TYPE_NONE) {
        return NULL;
    }
    return &table->entries[fd];
}

/* ---- the stack's state ---- */

static int started, crc_open, compat;
static struct evdev *mouse, *pad;

/* debugfs, mounted once, and the node each descriptor opened on it holds,
 * as the system call layer's open file holds its node. */
static struct vfs_mount dfs_mnt;
static void *dfs_held[FDS];

/* The files of debugfs a program opens: the DRM devices', under each
 * device's own directory, its unique name, which the dri/MINOR links name,
 * and sync's. */
static const char *const dfs_paths[] = {
    "sync/sw_sync",
    "sync/info",
    "dri/vkms/name",
    "dri/vkms/clients",
    "dri/vkms/gem_names",
    "dri/vkms/framebuffer",
    "dri/vkms/state",
    "dri/vkms/crtc-0/crc/control",
    "dri/vkms/crtc-0/crc/data",
    "dri/vkms/encoder-0/bridges",
    "dri/vkms/Virtual-1/force",
    "dri/vkms/Virtual-1/edid_override",
    "dri/fuzzdrm/name",
    "dri/fuzzdrm/clients",
    "dri/fuzzdrm/gem_names",
};

#define DFS_PATHS ((unsigned)(sizeof(dfs_paths) / sizeof(dfs_paths[0])))

/** The node of debugfs at `path`, held, as the walk finds it a name at a
 * time; NULL when a name is not there. */
static void *dfs_walk(const char *path)
{
    char name[64];
    void *dir = NULL, *node = NULL;
    unsigned n;

    while (*path) {
        for (n = 0; path[n] && path[n] != '/' && n < sizeof(name) - 1; n++) {
            name[n] = path[n];
        }
        name[n] = '\0';
        path += n + (path[n] == '/');
        if (debugfs_vops.lookup(&dfs_mnt, dir, name, &node) != 0) {
            node = NULL;
        }
        if (dir) {
            debugfs_vops.put(&dfs_mnt, dir);
        }
        if (!node) {
            return NULL;
        }
        dir = node;
    }
    return node;
}

/** The one process, by its PID, for drm_debugfs's clients file. */
process_t *get_process(uint32_t pid)
{
    return pid == self.pid ? &self : NULL;
}

int in_compat_syscall(void)
{
    return compat;
}

long ioctl_fd(int fd, uint32_t request, uintptr_t arg)
{
    fd_entry_t *e = fd_get(&fd_table, fd);

    if (!e) {
        return -EBADF;
    }
    if (!e->ops || !e->ops->ioctl) {
        return -ENOTTY;
    }
    return e->ops->ioctl(e, request, arg);
}

/** The compat path's native call on a structure a translation built in
 * kernel memory, as ia32_ioctl.c makes it. */
long ia32_ioctl_native(unsigned int fd, unsigned int cmd, void *buf, uint32_t len)
{
    long rc;

    user_window_open(buf, len);
    rc = ioctl_fd((int)fd, cmd, (uintptr_t)buf);
    user_window_close();
    return rc;
}

/* An event node's operations, as devfs gives them. */
static const struct file_ops evdev_fuzz_ops = {evdev_read,  NULL, evdev_poll, evdev_ioctl,
                                               evdev_close, NULL, NULL};

/* The second device: a render node with sync objects and PRIME, which
 * vkms, as Linux's, has neither of; dumb buffers of the shmem helper. */
static const struct drm_driver fuzz_driver = {
    {"fuzzdrm", "the harness's render device", "20261110", 1, 0, 0,
     DRIVER_GEM | DRIVER_RENDER | DRIVER_SYNCOBJ | DRIVER_SYNCOBJ_TIMELINE, DRM_BUS_PLATFORM},
    NULL,
    NULL,
    drm_gem_shmem_dumb_create,
    NULL,
    NULL,
    NULL,
    0,
    NULL};
static struct drm_device fuzz_dev;

/** Probe vkms, card 0, register the render device, card 1 and render node
 * 129, and a mouse and a touchpad, once. */
static void start(void)
{
    static uint8_t keys[EVDEV_KEY_BYTES];
    unsigned i;

    if (started) {
        return;
    }
    started = 1;
    cpu_table[0].current = &self;
    self.pid = self.tgid = 100;
    self.tg = &self_tg;
    self.mm = &self_mm;
    self.fds = &fd_table;
    k_strncpy(self.comm, "fuzz", sizeof(self.comm));
    if (vkms_probe() < 0) {
        fprintf(stderr, "drm-ioctl-fuzz: vkms did not probe\n");
        abort();
    }
    drm_dev_init(&fuzz_dev, &fuzz_driver);
    if (drm_dev_register(&fuzz_dev) != 0) {
        fprintf(stderr, "drm-ioctl-fuzz: the render device did not register\n");
        abort();
    }
    keys[BTN_LEFT / 8] |= (uint8_t)(1u << (BTN_LEFT % 8));
    keys[BTN_RIGHT / 8] |= (uint8_t)(1u << (BTN_RIGHT % 8));
    mouse = evdev_register("fuzz mouse", EVDEV_BUS_I8042, 2, 5, keys);
    evdev_set_capability(mouse, EV_REL, REL_X);
    evdev_set_capability(mouse, EV_REL, REL_Y);
    evdev_set_capability(mouse, EV_REL, REL_WHEEL);
    evdev_set_prop(mouse, INPUT_PROP_POINTER);
    keys[BTN_TOUCH / 8] |= (uint8_t)(1u << (BTN_TOUCH % 8));
    keys[BTN_TOOL_FINGER / 8] |= (uint8_t)(1u << (BTN_TOOL_FINGER % 8));
    pad = evdev_register("fuzz pad", EVDEV_BUS_I8042, 2, 7, keys);
    evdev_set_abs(pad, ABS_X, 0, 4000, 4, 0);
    evdev_set_abs(pad, ABS_Y, 0, 3000, 4, 0);
    evdev_set_abs(pad, ABS_PRESSURE, 0, 255, 0, 0);
    evdev_set_abs(pad, ABS_MT_POSITION_X, 0, 4000, 0, 0);
    evdev_set_abs(pad, ABS_MT_POSITION_Y, 0, 3000, 0, 0);
    (void)evdev_init_slots(pad, 4, 0);
    evdev_set_prop(pad, INPUT_PROP_BUTTONPAD);
    if (debugfs_mount(&dfs_mnt, NULL, 0, NULL) != 0) {
        fprintf(stderr, "drm-ioctl-fuzz: debugfs did not mount\n");
        abort();
    }
    for (i = 0; i < DFS_PATHS; i++) {
        void *node = dfs_walk(dfs_paths[i]);

        if (!node) {
            fprintf(stderr, "drm-ioctl-fuzz: debugfs has no %s\n", dfs_paths[i]);
            abort();
        }
        debugfs_vops.put(&dfs_mnt, node);
    }
}

/** Let `frames` frames of a 60 Hz clock pass, a millisecond at a time:
 * vkms's vblanks, its composer and the commit queue run as they would. */
static void run_time(int frames)
{
    int i;

    for (i = 0; i < frames * 17; i++) {
        now_ns += 1000000;
        vkms_tick();
    }
    if (vkms_dev) {
        vkms_composer_run(vkms_dev);
    }
    while (drm_atomic_commit_work()) {
    }
}

/* ---- the program ---- */

/* The unread part of the input. */
struct cursor {
    const uint8_t *p;
    unsigned long n;
};

/** Take one byte from the input; past its end, 0. */
static unsigned take8(struct cursor *c)
{
    unsigned v = 0;

    if (c->n) {
        v = *c->p++;
        c->n--;
    }
    return v;
}

/** Take a little-endian 16-bit value from the input. */
static unsigned take16(struct cursor *c)
{
    unsigned v = take8(c);

    return v | take8(c) << 8;
}

/** Take a little-endian 32-bit value from the input. */
static uint32_t take32(struct cursor *c)
{
    uint32_t v = take16(c);

    return v | (uint32_t)take16(c) << 16;
}

/** The descriptor a byte names among the open ones, or -1. */
static int pick_fd(unsigned b)
{
    int i, n = 0;

    for (i = 0; i < FDS; i++) {
        n += fd_entries[i].type != FD_TYPE_NONE;
    }
    if (n == 0) {
        return -1;
    }
    b %= (unsigned)n;
    for (i = 0; i < FDS; i++) {
        if (fd_entries[i].type != FD_TYPE_NONE && b-- == 0) {
            return i;
        }
    }
    return -1;
}

/** Open what byte `b` names: the card, the render node, a sw_sync
 * timeline, or the mouse's or the touchpad's event node. */
static void op_open(unsigned b)
{
    struct devfs_node *node = NULL;
    fd_entry_t *e;
    int fd;
    int32_t rc;

    switch (b % 6) {
    case 0:
        node = node_named("dri/card0");
        break;
    case 1:
        node = node_named("dri/card1");
        break;
    case 2:
        node = node_named("dri/renderD129");
        break;
    }
    fd = fd_alloc(&fd_table);
    if (fd < 0) {
        return;
    }
    e = &fd_entries[fd];
    e->flags = FD_FLAG_READ | FD_FLAG_WRITE;
    e->ofd = (struct open_file *)calloc(1, sizeof(struct open_file));
    e->ofd->refcount = 1;
    e->ofd->status_flags = OFD_NONBLOCK;
    if (b % 6 < 3) {
        e->type = FD_TYPE_DEVICE;
        e->ops = node->ops;
        rc = node->open(node, e, (b & 0x80) ? 0200 : 0);
    } else if (b % 6 == 3) {
        rc = sw_sync_open(e);
    } else {
        rc = evdev_open(e, b % 6 == 4 ? 0 : 1);
        if (rc == 0) {
            e->type = FD_TYPE_DEVICE;
            e->ops = &evdev_fuzz_ops;
        }
    }
    if (rc != 0) {
        free(e->ofd);
        fd_free(&fd_table, fd);
    }
}

/** Open the file of debugfs that byte `b` names, as the system call
 * layer binds a regular file of the mount. */
static void op_debugfs(unsigned b)
{
    void *node = dfs_walk(dfs_paths[b % DFS_PATHS]);
    fd_entry_t *e;
    int fd;

    if (!node) {
        return;
    }
    fd = fd_alloc(&fd_table);
    if (fd < 0) {
        debugfs_vops.put(&dfs_mnt, node);
        return;
    }
    e = &fd_entries[fd];
    e->type = FD_TYPE_FILE;
    e->flags = FD_FLAG_READ | FD_FLAG_WRITE;
    e->ofd = (struct open_file *)calloc(1, sizeof(struct open_file));
    e->ofd->refcount = 1;
    e->ofd->status_flags = OFD_NONBLOCK;
    if (debugfs_bind(node, e, 2) != 0) {
        free(e->ofd);
        fd_free(&fd_table, fd);
        debugfs_vops.put(&dfs_mnt, node);
        return;
    }
    dfs_held[fd] = node;
}

/** Close descriptor `fd`: its object's `close` with the description's
 * last descriptor, and the debugfs node it held. */
static void op_close(int fd)
{
    fd_entry_t *e = &fd_entries[fd];

    if (e->ops && e->ops->close) {
        e->ops->close(e);
    }
    free(e->ofd);
    fd_free(&fd_table, fd);
    if (dfs_held[fd]) {
        debugfs_vops.put(&dfs_mnt, dfs_held[fd]);
        dfs_held[fd] = NULL;
    }
}

/** Linux's ioctl number of direction `dir`, type `type`, number `nr` and
 * argument size `size`. */
static uint32_t ioc(unsigned dir, unsigned type, unsigned nr, unsigned size)
{
    return (uint32_t)dir << 30 | (uint32_t)(size & 0x3fff) << 16 | (uint32_t)type << 8 | nr;
}

/** An ioctl on a descriptor the program names, with a command it builds
 * and an argument of the bytes after it. */
static void op_ioctl(struct cursor *c)
{
    static const unsigned char types[] = {'d', 'E', '>', 'b', 'W'};
    int fd = pick_fd(take8(c));
    unsigned t = take8(c), len;
    uint32_t cmd;
    fd_entry_t *e;

    if (t % 6 == 5) {
        cmd = take32(c);
    } else {
        unsigned nr = take8(c), d = take16(c);

        cmd = ioc(d >> 14, types[t % 6], nr, d & 0x3fff);
    }
    len = take16(c) % (ARG_MAX + 1);
    memset(arena + ARG_OFF, 0, ARG_MAX);
    if (len > c->n) {
        len = (unsigned)c->n;
    }
    memcpy(arena + ARG_OFF, c->p, len);
    c->p += len;
    c->n -= len;
    if (fd < 0) {
        return;
    }
    e = &fd_entries[fd];
    if (e->ops && e->ops->ioctl) {
        long rc;

        unsigned type = cmd >> 8 & 0xff;

        if (compat && type == 'd') {
            rc = ia32_ioctl_drm((unsigned)fd, cmd, ARG_OFF);
        } else if (compat && (type == 'b' || type == '>' || type == 'u')) {
            rc = ia32_ioctl_gpu((unsigned)fd, cmd, ARG_OFF);
        } else {
            rc = e->ops->ioctl(e, cmd, ARG_OFF);
        }
        if (FUZZ_LOG) {
            fprintf(stderr, "drm-ioctl-fuzz: fd %d %sioctl %#x: %ld\n", fd, compat ? "i386 " : "",
                    (unsigned)cmd, rc);
        }
    }
}

/** A write of the bytes that follow, up to a count the program gives. */
static void op_write(struct cursor *c)
{
    int fd = pick_fd(take8(c));
    unsigned len = take16(c) % (ARG_MAX + 1);
    fd_entry_t *e;

    if (len > c->n) {
        len = (unsigned)c->n;
    }
    memcpy(arena + ARG_OFF, c->p, len);
    c->p += len;
    c->n -= len;
    if (fd < 0) {
        return;
    }
    e = &fd_entries[fd];
    if (e->ops && e->ops->write) {
        (void)e->ops->write(e, (const char __user *)(uintptr_t)ARG_OFF, len);
    }
}

/** A read of up to a count the program gives, never blocking. */
static void op_read(struct cursor *c)
{
    int fd = pick_fd(take8(c));
    unsigned n = take16(c) % (READ_MAX + 1);
    fd_entry_t *e;

    if (fd < 0) {
        return;
    }
    e = &fd_entries[fd];
    if (e->ops && e->ops->read) {
        (void)e->ops->read(e, (char __user *)(uintptr_t)READ_OFF, n);
    }
}

/** A poll of a descriptor. */
static void op_poll(struct cursor *c)
{
    int fd = pick_fd(take8(c));
    struct wait_queue *wq = NULL;
    fd_entry_t *e;

    if (fd < 0) {
        return;
    }
    e = &fd_entries[fd];
    if (e->ops && e->ops->poll) {
        (void)e->ops->poll(e, (short)(POLLIN | POLLOUT), &wq);
    }
}

/** An event of the mouse or the touchpad and its report's end. */
static void op_event(struct cursor *c)
{
    struct evdev *dev = take8(c) & 1 ? pad : mouse;
    unsigned type = take8(c) % EV_CNT, code = take16(c);
    int32_t value = (int32_t)take32(c);

    evdev_report(dev, (uint16_t)type, (uint16_t)code, value);
    evdev_report(dev, EV_SYN, SYN_REPORT, 0);
}

/** The CRC files of vkms's CRTC, as debugfs drives them. */
static void op_crc(struct cursor *c)
{
    char buf[256];
    unsigned what = take8(c), len;

    if (!vkms_dev) {
        return;
    }
    switch (what % 4) {
    case 0:
        len = take8(c) % 16;
        if (len > c->n) {
            len = (unsigned)c->n;
        }
        memcpy(buf, c->p, len);
        c->p += len;
        c->n -= len;
        (void)drm_crtc_crc_set_source(&vkms_dev->crtc, buf, len);
        break;
    case 1:
        if (drm_crtc_crc_open(&vkms_dev->crtc) == 0) {
            crc_open++;
        }
        break;
    case 2:
        if (crc_open) {
            drm_crtc_crc_release(&vkms_dev->crtc);
            crc_open--;
        }
        break;
    case 3:
        if (crc_open) {
            (void)drm_crtc_crc_read(&vkms_dev->crtc, buf, take8(c), 1);
        }
        break;
    }
}

/** The step of the rarer operations, by the byte that follows: a CRC
 * file's call, an input event, an open of a debugfs file or a write. */
static void op_other(struct cursor *c)
{
    switch (take8(c) % 4) {
    case 0:
        op_crc(c);
        break;
    case 1:
        op_event(c);
        break;
    case 2:
        op_debugfs(take8(c));
        break;
    case 3:
        op_write(c);
        break;
    }
}

int LLVMFuzzerTestOneInput(const unsigned char *data, unsigned long size)
{
    struct cursor c;
    unsigned flags, len;
    int fd;

    c.p = data;
    c.n = size;
    flags = take8(&c);
    no_cursor = (flags & 2) != 0;
    compat = (flags & 4) != 0;
    start();
    self.ruid = self.euid = self.suid = (flags & 1) ? 0 : 1000;
    self.rgid = self.egid = self.sgid = (flags & 1) ? 0 : 1000;
    memset(arena, 0, sizeof(arena));
    len = take16(&c) % (ARENA - DATA_OFF + 1);
    if (len > c.n) {
        len = (unsigned)c.n;
    }
    memcpy(arena + DATA_OFF, c.p, len);
    c.p += len;
    c.n -= len;
    while (c.n) {
        switch (take8(&c) % 9) {
        case 0:
        case 1:
            op_open(take8(&c));
            break;
        case 2:
        case 3:
            op_ioctl(&c);
            break;
        case 4:
            fd = pick_fd(take8(&c));
            if (fd >= 0) {
                op_close(fd);
            }
            break;
        case 5:
            op_read(&c);
            break;
        case 6:
            op_poll(&c);
            break;
        case 7:
            op_other(&c);
            break;
        case 8:
            run_time(1 + (int)(take8(&c) % 8));
            break;
        }
        while (drm_atomic_commit_work()) {
        }
    }
    for (fd = 0; fd < FDS; fd++) {
        if (fd_entries[fd].type != FD_TYPE_NONE) {
            op_close(fd);
        }
    }
    while (crc_open) {
        drm_crtc_crc_release(&vkms_dev->crtc);
        crc_open--;
    }
    run_time(2);
    if (irq_depth != 0) {
        fprintf(stderr, "drm-ioctl-fuzz: interrupts left off\n");
        abort();
    }
    return 0;
}
