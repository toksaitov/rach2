/*
 * fat32_hosttest.c: the vfat driver over the buffer cache over an image.
 *
 * host-deps: rach2/sys/fs/fat32.c rach2/sys/fs/bcache.c rach2/sys/block/chain.c
 * host-deps: rach2/sys/fs/fat32name.c rach2/sys/fs/fatcommon.c
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-deps: tests/host/compat/kheap_host.c tests/host/compat/devmodel.c
 * host-deps: tests/host/compat/pressure_host.c tests/host/compat/fixdir_host.c
 * host-seconds: 300
 *
 * The volumes are mkfatfixture's, built here on first use the way the bootctl
 * test builds them, in the test's own fixture directory (compat/fixdir_host.c),
 * or in the one a mutation proof names with FIXDIR, so that runs at once do not
 * share one. The well-formed volume (`--vfs`) must read back exactly what the
 * builder wrote: the long names, the one across a 4 KiB boundary, the surrogate
 * pair, the bad checksum falling back to a lower-cased short name, the 0x05
 * short name reachable by its long name, the fragmented file, the empty file,
 * `..` back to the root, and the bytes of every file. The malformed volumes are
 * refused at mount or fail at the file they damage, under AddressSanitizer. A
 * create, a mkdir and a rename on a second volume, each with the same operation
 * on the first inside one of its writes, must each write the name it was given.
 * No hosted headers: the kernel headers define the fixed-width types.
 *
 * With `--barriers` the program runs only the interruption matrix and the
 * durability rows, the rows that judge the driver's barriers, and ends at
 * the first failure: the mode tests/host/fat_barriers_mutate.sh runs.
 */

#include "../../rach2/sys/fs/fat32.h"
#include "../../rach2/sys/fs/bcache.h"
#include "../../rach2/sys/lib/kerrno.h"
#include "../../rach2/sys/proc/process.h"
#include "devmodel.h"
#include "fixdir_host.h"

extern int printf(const char *fmt, ...);
extern int snprintf(char *s, unsigned long n, const char *fmt, ...);
extern int system(const char *cmd);
extern int access(const char *path, int mode);
extern void exit(int status);
extern void *fopen(const char *path, const char *mode);
extern unsigned long fread(void *buf, unsigned long size, unsigned long n, void *f);
extern unsigned long fwrite(const void *buf, unsigned long size, unsigned long n, void *f);
extern int fclose(void *f);

#define IMG_BYTES (8192u * 512u)

/* ---- stubs for what the driver and the cache reach outside themselves ---- */

process_t proc_a;
struct percpu cpu_table[NR_CPUS] = {{.current = &proc_a}};

void k_panic(const char *s)
{
    printf("fat32_hosttest: panic: %s\n", s);
    exit(3);
}

static char last_log[128];
extern int vsnprintf(char *s, unsigned long n, const char *fmt, __builtin_va_list ap);

void klog(int level, const char *sys, const char *fmt, ...)
{
    __builtin_va_list ap;

    (void)level;
    (void)sys;
    __builtin_va_start(ap, fmt);
    vsnprintf(last_log, sizeof(last_log), fmt, ap);
    __builtin_va_end(ap);
}

void wait_queue_init(wait_queue_t *wq)
{
    __builtin_memset(wq, 0, sizeof(*wq));
}
void wq_wake_all(wait_queue_t *wq)
{
    (void)wq;
}
int wq_wait_event(wait_queue_t *wq, wq_cond_fn cond, void *arg)
{
    (void)wq;
    (void)cond;
    (void)arg;
    return -1;
}

/* ---- the image as a block device ---- */

/* `image` is what the device answers, its medium with the writes pending
   since the last barrier over it; `medium` is what a crash keeps, through
   the model in tests/host/compat/devmodel.c. */
static unsigned char image[IMG_BYTES];
static unsigned char medium[IMG_BYTES];
static unsigned char image_before[IMG_BYTES];
static unsigned char snap[IMG_BYTES];
static unsigned char snaps[DEVMODEL_KEEP_MOST + 2][IMG_BYTES]; /* the states at one stop */
static struct devmodel model;
static int reads_made;

static int img_read(block_device_t *d, uint64_t lba, uint32_t count, void *buf)
{
    (void)d;
    reads_made++;
    return devmodel_read(&model, lba, count, buf);
}

/* Writes go to the model, pending until the driver's next barrier.
   `stop_after` is the interruption matrix's crash: the writes after that
   many fail, as a machine stopped after the Nth write would, and what the
   crash leaves is the model's snapshot. */
static int writes_made;
static int stop_after = -1;

static int img_write(block_device_t *d, uint64_t lba, uint32_t count, const void *buf)
{
    (void)d;
    int rc;

    if (stop_after >= 0 && writes_made >= stop_after) {
        return -5;
    }
    writes_made++;
    rc = devmodel_write(&model, lba, count, buf);
    if (stop_after >= 0 && writes_made >= stop_after) {
        model.stopped = 1; /* the Nth write is the last thing that lands */
    }
    return rc;
}

/* The medium as it stands, with nothing pending: what the tests that judge
   the image directly see, since a barrier came before them. */
static void settle(void)
{
    if (model.npending) {
        devmodel_barrier(&model);
    }
}

static block_device_t dev = {512, 8192u, BLOCK_FAULT_SAFE, 0, 0, 0, img_read, img_write, 0, 0, 0};

int block_request(block_device_t *device, int write, uint64_t lba, uint32_t count, void *data)
{
    if (write) {
        return device->write(device, lba, count, data);
    }
    return device->read(device, lba, count, data);
}

/* ---- the rows ---- */

static int fails;
/* Set by the mode a mutation proof runs: the first failure ends the run,
   since the proof asks only whether a row fails. */
static int first_failure_ends;

static void check(int ok, const char *what)
{
    if (!ok) {
        fails++;
        printf("FAIL: %s\n", what);
        if (first_failure_ends) {
            exit(1);
        }
    }
}

static int streq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static int builder_built;

/** Build the named fixture and load it into the image. */
static void load(const char *opt)
{
    char cmd[2048];
    void *f;

    /* Built once per run, so an edited builder is the one that runs. */
    if (!builder_built) {
        fix_format(cmd, sizeof(cmd), "cc -O2 -o %s/mkfatfixture tests/host/mkfatfixture.c",
                   fix_dir());
        if (system(cmd) != 0) {
            printf("fat32_hosttest: cannot build the fixture builder\n");
            exit(1);
        }
        builder_built = 1;
    }
    fix_format(cmd, sizeof(cmd), "%s/mkfatfixture %s/fat32_case.img %s", fix_dir(), fix_dir(), opt);
    if (system(cmd) != 0) {
        printf("fat32_hosttest: fixture builder failed for %s\n", opt);
        exit(1);
    }
    /* Whatever the cache still holds of the previous image goes into that
       image before it is replaced, so the cache empties; a stop is over. */
    stop_after = -1;
    model.stopped = 0;
    bcache_sync(&dev);
    f = fopen(fix_path("fat32_case.img"), "rb");
    if (!f || fread(image, 1, IMG_BYTES, f) != IMG_BYTES) {
        printf("fat32_hosttest: fixture did not load\n");
        exit(1);
    }
    fclose(f);
    if (bcache_invalidate(&dev) != 0) {
        printf("fat32_hosttest: the cache did not empty for a new fixture\n");
        exit(1);
    }
    devmodel_load(&model);
}

static struct vfs_mount mnt;

/* Mount the loaded image read-only with `options`; the constructor's rc. */
static int mount_image(const char *options)
{
    int rc;

    __builtin_memset(&mnt, 0, sizeof(mnt));
    mnt.type = &vfs_vfat_type;
    mnt.dev = &dev;
    mnt.flags = VFS_MS_RDONLY;
    mnt.ns_holder = -1;
    rc = vfs_vfat_type.mount(&mnt, &dev, VFS_MS_RDONLY, options);
    if (rc == 0) {
        vfs_ns_lock(&mnt); /* the test stands in for the syscall layer */
    }
    return rc;
}

static void unmount_image(void)
{
    if (mnt.root) {
        vfs_vfat_type.ops->put(&mnt, mnt.root);
        vfs_vfat_type.ops->unmount(&mnt);
    }
}

#define OPS (vfs_vfat_type.ops)

/* Resolve `path` under the root by lookup per component; the node, or NULL. */
static void *walk(const char *path, int *err)
{
    void *cur = mnt.root;
    char comp[256];

    OPS->hold(&mnt, cur);
    *err = 0;
    while (*path) {
        int n = 0;
        void *next;
        int rc;

        while (*path == '/') {
            path++;
        }
        if (!*path) {
            break;
        }
        while (*path && *path != '/') {
            comp[n++] = *path++;
        }
        comp[n] = '\0';
        rc = OPS->lookup(&mnt, cur, comp, &next);
        OPS->put(&mnt, cur);
        if (rc != 0) {
            *err = rc;
            return 0;
        }
        cur = next;
    }
    return cur;
}

/* Read a whole file into `buf`; the byte count, or a negative errno. */
static int read_all(void *node, unsigned char *buf, uint32_t cap)
{
    void *h;
    int32_t got, total = 0;

    if (OPS->open(&mnt, node, 0, &h) != 0) {
        return -1;
    }
    for (;;) {
        got = OPS->read(&mnt, h, buf + total, cap - (uint32_t)total, (uint64_t)total);
        if (got < 0) {
            OPS->release(&mnt, h);
            return got;
        }
        if (got == 0) {
            break;
        }
        total += got;
    }
    OPS->release(&mnt, h);
    return total;
}

/* Count the entries a directory lists and note whether `name` is among
   them; a listing that ends in an error returns that errno, and every inode
   number listed goes into `inos` (at most 64) when it is given. */
static uint64_t inos[64];
static int ino_count;

static int list_dir(void *dir, const char *name, int *seen)
{
    void *h;
    uint32_t cursor = 0;
    int count = 0;
    int got;
    struct vfs_dirent de;

    *seen = 0;
    if (OPS->open(&mnt, dir, 0, &h) != 0) {
        return -1;
    }
    while ((got = OPS->readdir(&mnt, h, &cursor, &de)) == 1) {
        count++;
        if (streq(de.name, name)) {
            *seen = 1;
        }
        if (ino_count < 64 && !streq(de.name, ".") && !streq(de.name, "..")) {
            inos[ino_count++] = de.ino;
        }
    }
    OPS->release(&mnt, h);
    return got < 0 ? got : count;
}

/* Every inode number collected so far is distinct. */
static int inos_distinct(void)
{
    int i, j;

    for (i = 0; i < ino_count; i++) {
        for (j = i + 1; j < ino_count; j++) {
            if (inos[i] == inos[j]) {
                printf("fat32_hosttest: entries %d and %d share inode %lu of %d listed\n", i, j,
                       (unsigned long)inos[i], ino_count);
                return 0;
            }
        }
    }
    return 1;
}

static void well_formed_rows(void)
{
    unsigned char buf[2048];
    struct vfs_stat st;
    void *n, *sub, *up;
    int err, seen, count, i, ok;
    static const char crossing[] =
        "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz"
        "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz"
        "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuv.txt";

    load("--vfs");
    check(mount_image("umask=022,uid=7,gid=9") == 0, "the well-formed volume mounts");

    /* Long names, case folding, and the bytes behind them. */
    n = walk("/long name with spaces.txt", &err);
    check(n != 0, "a long name is found");
    check(n && read_all(n, buf, sizeof(buf)) == 27 &&
              __builtin_memcmp(buf, "the long-named file's bytes", 27) == 0,
          "and reads its bytes");
    if (n) {
        OPS->put(&mnt, n);
    }
    n = walk("/LONG NAME WITH SPACES.TXT", &err);
    check(n != 0, "a long name matches with ASCII case folded");
    if (n) {
        OPS->put(&mnt, n);
    }
    n = walk("/longna~1.txt", &err);
    check(n != 0, "the short alias is reachable too");
    if (n) {
        OPS->put(&mnt, n);
    }

    n = walk("/\xF0\x9F\x98\x80.txt", &err);
    check(n != 0, "a surrogate pair becomes one UTF-8 character");
    check(n && read_all(n, buf, sizeof(buf)) == 5 && __builtin_memcmp(buf, "smile", 5) == 0,
          "and the file behind it reads");
    if (n) {
        OPS->put(&mnt, n);
    }

    n = walk("/should-not-appear.txt", &err);
    check(n == 0 && err == -ENOENT, "a long name with a bad checksum is not a name");
    n = walk("/badsum.txt", &err);
    check(n != 0, "its short name is shown, lower-cased by the case bits");
    check(n && read_all(n, buf, sizeof(buf)) == 6 && __builtin_memcmp(buf, "badsum", 6) == 0,
          "and reads");
    if (n) {
        OPS->put(&mnt, n);
    }

    n = walk("/e5file.txt", &err);
    check(n != 0, "a 0x05 short name is reachable by its long name");
    if (n) {
        OPS->put(&mnt, n);
    }
    n = walk("/?5file.txt", &err);
    check(n != 0, "and by its short alias, the byte above 0x7F shown as ?");
    if (n) {
        OPS->put(&mnt, n);
    }
    ino_count = 0;
    count = list_dir(mnt.root, "e5file.txt", &seen);
    check(seen, "the listing shows the long name");
    check(count == 15, "the root lists fifteen entries: no label, no deleted entry");

    /* Long names no writer produces are not names; the alias is shown. */
    check(walk("/badord.txt", &err) != 0 && list_dir(mnt.root, "BADORD.TXT", &seen) > 0 && seen,
          "a long name with a reserved ordinal bit is shown by its alias");
    check(walk("/badattr.txt", &err) != 0 && list_dir(mnt.root, "BADATTR.TXT", &seen) > 0 && seen,
          "a long name with a wrong attribute byte is shown by its alias");
    check(walk("/badpad.txt", &err) != 0 && list_dir(mnt.root, "BADPAD.TXT", &seen) > 0 && seen,
          "a long name with a unit past its terminator is shown by its alias");
    check(walk("/lone.txt", &err) != 0 && list_dir(mnt.root, "LONE.TXT", &seen) > 0 && seen,
          "a long name with an unpaired surrogate is shown by its alias");
    ino_count = 0;
    list_dir(mnt.root, "", &seen);
    n = walk("/sub", &err);
    if (n) {
        list_dir(n, "", &seen);
        OPS->put(&mnt, n);
    }
    n = walk("/many", &err);
    if (n) {
        list_dir(n, "", &seen);
        OPS->put(&mnt, n);
    }
    check(ino_count == 58 && inos_distinct(),
          "no two of the 58 entries listed share an inode number");
    /* The four walks above put their nodes back through the pool row's
       release; here each is put at once. */
    for (i = 0; i < 4; i++) {
        static const char *const aliases[] = {"/badord.txt", "/badattr.txt", "/badpad.txt",
                                              "/lone.txt"};

        n = walk(aliases[i], &err);
        if (n) {
            OPS->put(&mnt, n);
            OPS->put(&mnt, n);
        }
    }

    /* Times: local time read as UTC, hundredths added to the creation
       second, and a field out of range reading as 1980-01-01. */
    n = walk("/stamp.txt", &err);
    check(n && OPS->getattr(&mnt, n, &st) == 0 && st.mtime_sec == 946684798 &&
              st.ctime_sec == 1583323201 && st.ctime_nsec == 500000000,
          "the stamped file reports its creation and modification times");
    if (n) {
        OPS->put(&mnt, n);
    }
    n = walk("/baddate.txt", &err);
    check(n && OPS->getattr(&mnt, n, &st) == 0 && st.mtime_sec == 315532800,
          "a date naming month 13 reads as 1980-01-01");
    if (n) {
        OPS->put(&mnt, n);
    }

    n = walk(crossing, &err);
    check(n != 0, "a long name across the 4 KiB boundary is found");
    check(n && read_all(n, buf, sizeof(buf)) == 8 && __builtin_memcmp(buf, "crossing", 8) == 0,
          "and reads");
    if (n) {
        OPS->put(&mnt, n);
    }

    /* The fragmented file reads across its jump, whole and in pieces, and
       a second read of it is served by the cache. */
    n = walk("/frag.bin", &err);
    check(n != 0, "the fragmented file is found by its folded name");
    ok = n && read_all(n, buf, sizeof(buf)) == 1536;
    {
        int before = reads_made;

        check(ok && read_all(n, buf, sizeof(buf)) == 1536 && reads_made == before,
              "a second read of the file makes no device read");
    }
    for (i = 0; ok && i < 1536; i++) {
        if (buf[i] != (unsigned char)((i * 7) & 0xFF)) {
            ok = 0;
        }
    }
    check(ok, "and reads every byte across the jump");
    if (n) {
        void *h;

        OPS->open(&mnt, n, 0, &h);
        check(OPS->read(&mnt, h, buf, 100, 1500) == 36, "a read at the tail is cut to the size");
        check(OPS->read(&mnt, h, buf, 10, 1536) == 0, "and a read at the end is empty");
        check(OPS->read(&mnt, h, buf, 4, 1020) == 4 && buf[0] == (unsigned char)((1020 * 7) & 0xFF),
              "a read straddling the jump lands on the right cluster");
        OPS->release(&mnt, h);
        OPS->put(&mnt, n);
    }

    /* The empty file, the attributes, and the mount's identity. */
    n = walk("/BOOT/EXTRA/EMPTY", &err);
    check(n != 0, "the empty file is found");
    check(n && read_all(n, buf, sizeof(buf)) == 0, "and reads as empty");
    if (n && OPS->getattr(&mnt, n, &st) == 0) {
        check(st.size == 0 && (st.mode & 0777) == 0755 && st.uid == 7 && st.gid == 9,
              "a file reports the mount's owner and the umask's mode");
    }
    if (n) {
        OPS->put(&mnt, n);
    }
    if (OPS->getattr(&mnt, mnt.root, &st) == 0) {
        check((st.mode & 0170000) == 0040000 && (st.mode & 0777) == 0755,
              "a directory reports the umask's mode");
    }

    /* `..` back up: the subdirectory's parent is the root by cluster zero. */
    sub = walk("/sub", &err);
    check(sub != 0, "the subdirectory is found");
    if (sub) {
        check(OPS->lookup(&mnt, sub, "..", &up) == 0 && up == mnt.root, "its .. is the root node");
        if (up) {
            OPS->put(&mnt, up);
        }
        n = walk("/sub/inner.txt", &err);
        check(n != 0 && read_all(n, buf, sizeof(buf)) == 5, "a file inside it reads");
        if (n) {
            OPS->put(&mnt, n);
        }
        OPS->put(&mnt, sub);
    }
    n = walk("/BOOT/EXTRA", &err);
    if (n) {
        void *parent;

        check(OPS->lookup(&mnt, n, "..", &parent) == 0, "a deeper .. resolves");
        if (parent) {
            void *boot = walk("/BOOT", &err);

            check(parent == boot, "to the same node as its parent's own name");
            if (boot) {
                OPS->put(&mnt, boot);
            }
            OPS->put(&mnt, parent);
        }
        OPS->put(&mnt, n);
    }

    /* What the volume is. */
    {
        struct vfs_statfs sf;

        check(OPS->statfs(&mnt, &sf) == 0 && sf.bsize == 512 && sf.blocks == 8032 && sf.bfree > 0 &&
                  sf.bfree < 8032 && sf.magic == 0x4d44 && sf.namelen == 255,
              "statfs reports the cluster geometry, a free count, vfat's magic and name limit");
    }
    check(walk("/nosuch", &err) == 0 && err == -ENOENT, "a missing name is ENOENT");
    check(walk("/frag.bin/x", &err) == 0 && err == -ENOENT,
          "a file is not searched as a directory");
    unmount_image();
}

static void small_and_active_rows(void)
{
    unsigned char buf[64];
    void *n;
    int err;

    /* The default fixture is a valid FAT32 of 8032 clusters, far below the
       65525 the specification names for formatting; it must mount. */
    load("");
    check(mount_image("") == 0, "a valid FAT32 below 65525 clusters mounts");
    n = walk("/BOOT/GRUB/GRUB.CFG", &err);
    check(n != 0 && read_all(n, buf, sizeof(buf)) == 40 && buf[0] == 'x', "and a file on it reads");
    if (n) {
        OPS->put(&mnt, n);
    }
    unmount_image();

    /* Mirroring off, the second FAT active, the first wrong: the kernel
       file reads only when the active FAT is the one followed. */
    load("--active-second");
    check(mount_image("") == 0, "the active-second volume mounts");
    n = walk("/BOOT/KERNEL", &err);
    check(n != 0, "the kernel file is found");
    if (n) {
        void *h;
        int32_t got;
        static unsigned char big[4096];

        OPS->open(&mnt, n, 0, &h);
        got = OPS->read(&mnt, h, big, sizeof(big), 4 * 512);
        check(got == (int32_t)sizeof(big),
              "and reads through the second FAT, not the first's wrong entries");
        OPS->release(&mnt, h);
        OPS->put(&mnt, n);
    }
    unmount_image();

    /* The reserved high bits of a FAT entry are masked before it is
       judged. */
    load("--unmasked");
    check(mount_image("") == 0, "the unmasked volume mounts");
    n = walk("/BOOT/KERNEL", &err);
    check(n != 0, "the kernel file on the unmasked volume is found");
    if (n) {
        void *h;
        static unsigned char big[4096];

        OPS->open(&mnt, n, 0, &h);
        check(OPS->read(&mnt, h, big, sizeof(big), 8 * 512) == (int32_t)sizeof(big),
              "a chain whose entries carry high bits still reads");
        OPS->release(&mnt, h);
        OPS->put(&mnt, n);
    }
    unmount_image();
}

/* Patch `width` bytes at `off` of the loaded volume's boot sector to
   `value` and mount: the constructor must answer EINVAL and name `field`. */
static void bpb_row(unsigned off, int width, uint32_t value, const char *field)
{
    char want[96];
    int i;

    load("--vfs");
    for (i = 0; i < width; i++) {
        image[off + (unsigned)i] = (unsigned char)(value >> (8 * i));
    }
    if (off == 32) {
        image[19] = 0; /* the 32-bit total counts only when the 16-bit one is zero */
        image[20] = 0;
    }
    bcache_invalidate(&dev);
    last_log[0] = '\0';
    snprintf(want, sizeof(want), "refused: %s", field);
    check(mount_image("") == -EINVAL && streq(last_log, want), field);
    if (!streq(last_log, want)) {
        printf("fat32_hosttest: the log said '%s'\n", last_log);
    }
}

static void malformed_rows(void)
{
    void *n;
    int err;
    static unsigned char big[8192];

    bpb_row(510, 1, 0, "boot-sector signature");
    bpb_row(22, 2, 1, "not a FAT32 volume");
    bpb_row(36, 4, 0, "not a FAT32 volume");
    bpb_row(17, 2, 512, "not a FAT32 volume");
    bpb_row(42, 2, 1, "not a FAT32 volume");
    bpb_row(11, 2, 1024, "sector size");
    bpb_row(13, 1, 0, "sectors per cluster");
    bpb_row(13, 1, 3, "sectors per cluster");
    bpb_row(13, 1, 128, "sectors per cluster");
    bpb_row(14, 2, 0, "reserved sectors");
    bpb_row(16, 1, 3, "FAT count");
    bpb_row(32, 4, 0, "total sectors");
    bpb_row(32, 4, (uint32_t)dev.sector_count + 1, "total sectors");
    bpb_row(14, 2, 0xFFFF, "FAT regions");
    bpb_row(44, 4, 1, "root cluster");
    bpb_row(44, 4, 0x0FFFFFF0, "root cluster");
    bpb_row(40, 2, 0x0085, "active FAT");
    {
        uint64_t saved = dev.sector_count;

        /* Sectors enough for a cluster count no 28-bit FAT can name. */
        dev.sector_count = 0xFFFFFFFFu;
        bpb_row(32, 4, 0xFFFFFFF0u, "cluster count");
        dev.sector_count = saved;
    }

    load("--bpb-sector-size");
    check(mount_image("") == -EINVAL, "a 4096-byte sector volume is refused");
    load("--bpb-root-out");
    check(mount_image("") == -EINVAL, "a root cluster past the data region is refused");
    load("--bpb-fat-oversized");
    check(mount_image("") == -EINVAL, "a FAT larger than the volume is refused");
    load("--bpb-overflow");
    check(mount_image("") == -EINVAL, "a reserved count that wraps is refused");

    load("--dir-loop");
    check(mount_image("") == 0, "a volume with a looped directory mounts");
    n = walk("/sub", &err);
    check(n != 0, "the looped directory is found");
    if (n) {
        void *h;
        uint32_t cursor = 0;
        struct vfs_dirent de;
        int got;

        OPS->open(&mnt, n, 0, &h);
        do {
            got = OPS->readdir(&mnt, h, &cursor, &de);
        } while (got == 1);
        check(got == -EIO, "and listing it ends in EIO at the volume's bound, not never");
        OPS->release(&mnt, h);
        OPS->put(&mnt, n);
    }
    n = walk("/sub/outrange.bin", &err);
    check(n != 0, "the file with a link outside the data region is found");
    if (n) {
        void *h;

        OPS->open(&mnt, n, 0, &h);
        check(OPS->read(&mnt, h, big, 512, 0) == 512, "the damaged file reads its sound clusters");
        check(OPS->read(&mnt, h, big, 512, 1024) == -EIO,
              "and a read reaching the cluster outside the data region fails with EIO");
        OPS->release(&mnt, h);
        OPS->put(&mnt, n);
    }
    n = walk("/sub/shortch.bin", &err);
    check(n != 0, "the file with the short chain is found");
    if (n) {
        void *h;

        OPS->open(&mnt, n, 0, &h);
        check(OPS->read(&mnt, h, big, 512, 512) == -EIO,
              "a read past a chain shorter than the size fails with EIO");
        OPS->release(&mnt, h);
        OPS->put(&mnt, n);
    }
    unmount_image();

    load("");
    {
        int rc1 = mount_image("umask=0");
        int rc2;

        unmount_image();
        rc2 = mount_image("nosuch=1");
        if (rc1 != 0 || rc2 != -EINVAL) {
            printf("fat32_hosttest: umask=0 mounted %d, nosuch=1 mounted %d\n", rc1, rc2);
        }
        check(rc1 == 0 && rc2 == -EINVAL, "an unknown option is EINVAL");
    }
    check(vfs_vfat_type.mount(&mnt, &dev, 0, "") == 0, "a writable mount is accepted");
    unmount_image();
}

/* ---- the write half ---- */

extern int system(const char *cmd);
extern void *popen(const char *cmd, const char *mode);
extern int pclose(void *f);
extern char *fgets(char *s, int n, void *f);
extern char *strstr(const char *hay, const char *needle);
extern int strncmp(const char *a, const char *b, unsigned long n);
extern unsigned long strlen(const char *s);

static int mount_rw(const char *options)
{
    int rc;

    __builtin_memset(&mnt, 0, sizeof(mnt));
    mnt.type = &vfs_vfat_type;
    mnt.dev = &dev;
    mnt.flags = 0;
    mnt.ns_holder = -1;
    rc = vfs_vfat_type.mount(&mnt, &dev, 0, options);
    if (rc == 0) {
        vfs_ns_lock(&mnt);
    }
    return rc;
}

/* The image to a file and the check over it; its output in `out`. */
static int run_check_on(const unsigned char *what, char *out, unsigned long cap)
{
    char cmd[2048], line[300];
    void *f;
    unsigned long n = 0;
    int rc;

    f = fopen(fix_path("fat32_write.img"), "wb");
    if (!f || fwrite(what, 1, IMG_BYTES, f) != IMG_BYTES) {
        printf("fat32_hosttest: cannot dump the image\n");
        exit(1);
    }
    fclose(f);
    fix_format(cmd, sizeof(cmd), "%s/mkfatfixture --check %s/fat32_write.img", fix_dir(),
               fix_dir());
    f = popen(cmd, "r");
    if (!f) {
        printf("fat32_hosttest: cannot run the check\n");
        exit(1);
    }
    out[0] = '\0';
    while (fgets(line, sizeof(line), f) && n + strlen(line) < cap - 1) {
        __builtin_memcpy(out + n, line, strlen(line) + 1);
        n += strlen(line);
    }
    rc = pclose(f);
    return rc == -1 ? -1 : (rc >> 8) & 0xff;
}

/* The image the device answers, settled: what a checker sees after a clean
   end, where every write reached the medium. */
static int run_check(char *out, unsigned long cap)
{
    settle();
    return run_check_on(image, out, cap);
}

/* The findings the driver's own order may leave behind a crash, and the
   dirty flag of a mounted volume; anything else is a fault. */
static int findings_allowed(const char *out)
{
    const char *p = out;

    while (*p) {
        const char *nl = strstr(p, "\n");
        unsigned long len = nl ? (unsigned long)(nl - p) : strlen(p);

        if (strncmp(p, "check: OK: ", 11) != 0 && strncmp(p, "check: FAIL: ", 13) != 0 &&
            strncmp(p, "check: dirty: ", 14) != 0 && strncmp(p, "check: lost: ", 13) != 0 &&
            strncmp(p, "check: surplus: ", 16) != 0 &&
            strncmp(p, "check: orphan long-name", 23) != 0 &&
            strncmp(p, "check: mirrors differ", 21) != 0 &&
            strncmp(p, "check: fsinfo: ", 15) != 0) {
            return 0;
        }
        p += len + (nl ? 1 : 0);
    }
    return 1;
}

static char check_out[16384];

/* The volume is consistent: no finding at all (`mounted` allows the dirty
   flag's two lines). */
static void check_consistent(const char *what, int mounted)
{
    int rc = run_check(check_out, sizeof(check_out));
    int ok;

    if (mounted) {
        const char *p = check_out;

        ok = 1;
        while (*p) {
            const char *nl = strstr(p, "\n");

            /* A mounted volume is dirty by design, and its FSInfo lags
               until a sync or the unmount writes it. */
            if (strncmp(p, "check: OK: ", 11) != 0 && strncmp(p, "check: FAIL: ", 13) != 0 &&
                strncmp(p, "check: dirty: ", 14) != 0 && strncmp(p, "check: fsinfo: ", 15) != 0) {
                ok = 0;
            }
            p += nl ? (unsigned long)(nl - p) + 1 : strlen(p);
        }
    } else {
        ok = rc == 0;
    }
    check(ok, what);
    if (!ok) {
        printf("fat32_hosttest: the check said:\n%s", check_out);
    }
}

static void *make_file(void *dir, const char *name)
{
    void *n = 0;

    if (OPS->create(&mnt, dir, name, 0644, &n) != 0) {
        return 0;
    }
    return n;
}

static int write_at(void *node, const void *buf, uint32_t n, uint64_t off)
{
    void *h;
    int32_t got;

    if (OPS->open(&mnt, node, 0, &h) != 0) {
        return -1;
    }
    got = OPS->write(&mnt, h, buf, n, off);
    OPS->release(&mnt, h);
    return got == (int32_t)n ? 0 : (got < 0 ? got : -1);
}

static unsigned char pattern[4096];
static unsigned char back[8192];

/* The names the sequences use, short or, in the matrix's second pass, long:
   every kind of long name is among them, with entries spanning a sector. */
static int long_names;

static const char *nm(const char *shortname)
{
    static const char *const map[][2] = {
        {"W1.TXT", "write one.txt"},
        {"W2.TXT", "Write Two.txt"},
        {"W3.TXT", "write three, a long name past one entry.txt"},
        {"W6.TXT", "Grow In Place.txt"},
        {"W4.TXT", "truncate me.txt"},
        {"W5.TXT", "unlink me.txt"},
        {"W7.TXT", "unlink me after a sync.txt"},
        {"W8.TXT", "truncate me after a sync.txt"},
        {"D1", "Dir One"},
        {"D2", "Dir Two"},
        {"R1.TXT", "rename one.txt"},
        {"R2.TXT", "rename two.txt"},
        {"R3.TXT", "rename three.txt"},
        {"RD", "Rename Dir"},
        {"RD2", "Rename Dir Two"},
        {"O1.TXT", "over one.txt"},
        {"O2.TXT", "over two.txt"},
        {"MANY", "Many Files"},
    };
    unsigned i;

    if (!long_names) {
        return shortname;
    }
    for (i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        if (streq(map[i][0], shortname)) {
            return map[i][1];
        }
    }
    return shortname;
}

static const char *nmf(int i)
{
    static char name[40];

    snprintf(name, sizeof(name), long_names ? "file number %02d.txt" : "F%02d", i);
    return name;
}

static void fill_pattern(void)
{
    unsigned i;

    for (i = 0; i < sizeof(pattern); i++) {
        pattern[i] = (unsigned char)((i * 13 + 7) & 0xFF);
    }
}

/* The sequences of the matrix: each stands alone on a fresh volume and
   returns the first error. */
static int seq_create_write(void)
{
    void *n = make_file(mnt.root, nm("W1.TXT"));
    int rc;

    if (!n) {
        return -1;
    }
    rc = write_at(n, "hello", 5, 0);
    OPS->put(&mnt, n);
    return rc;
}

static int seq_extend(void)
{
    void *n = make_file(mnt.root, nm("W2.TXT"));
    int rc;

    if (!n) {
        return -1;
    }
    rc = write_at(n, pattern, 600, 0);
    if (rc == 0) {
        rc = write_at(n, pattern + 600, 600, 600);
    }
    OPS->put(&mnt, n);
    return rc;
}

static int seq_past_end(void)
{
    void *n = make_file(mnt.root, nm("W3.TXT"));
    int rc;

    if (!n) {
        return -1;
    }
    rc = write_at(n, pattern, 10, 0);
    if (rc == 0) {
        rc = write_at(n, pattern + 10, 10, 2000);
    }
    OPS->put(&mnt, n);
    return rc;
}

/* Two writes into one cluster: the second grows the size over bytes that
   were on the disk as junk until the data reached it. */
static int seq_grow_in_place(void)
{
    void *n = make_file(mnt.root, nm("W6.TXT"));
    int rc;

    if (!n) {
        return -1;
    }
    rc = write_at(n, pattern, 100, 0);
    if (rc == 0) {
        rc = write_at(n, pattern + 100, 100, 100);
    }
    OPS->put(&mnt, n);
    return rc;
}

/* Twenty files in a new directory, which grows past its first cluster. */
static int seq_many_files(void)
{
    void *d;
    int i, rc = OPS->mkdir(&mnt, mnt.root, nm("MANY"), 0755);

    if (rc != 0) {
        return rc;
    }
    if (OPS->lookup(&mnt, mnt.root, nm("MANY"), &d) != 0) {
        return -1;
    }
    for (i = 0; i < 20 && rc == 0; i++) {
        void *n;

        rc = OPS->create(&mnt, d, nmf(i), 0644, &n);
        if (rc == 0) {
            rc = write_at(n, "f", 1, 0);
            OPS->put(&mnt, n);
        }
    }
    OPS->put(&mnt, d);
    return rc;
}

static int seq_truncate(void)
{
    void *n = make_file(mnt.root, nm("W4.TXT"));
    void *h;
    int rc;

    if (!n) {
        return -1;
    }
    rc = write_at(n, pattern, 2010, 0);
    if (rc == 0 && OPS->open(&mnt, n, 0, &h) == 0) {
        rc = OPS->truncate(&mnt, h, 700);
        if (rc == 0) {
            rc = OPS->truncate(&mnt, h, 1000); /* a grow inside the last cluster */
        }
        if (rc == 0) {
            rc = OPS->truncate(&mnt, h, 2600); /* and one across a cluster */
        }
        if (rc == 0) {
            rc = OPS->truncate(&mnt, h, 0);
        }
        if (rc == 0) {
            rc = OPS->truncate(&mnt, h, 1500);
        }
        OPS->release(&mnt, h);
    }
    OPS->put(&mnt, n);
    return rc;
}

static int seq_unlink(void)
{
    void *n = make_file(mnt.root, nm("W5.TXT"));
    int rc;

    if (!n) {
        return -1;
    }
    rc = write_at(n, pattern, 1000, 0);
    OPS->put(&mnt, n);
    if (rc == 0) {
        rc = OPS->unlink(&mnt, mnt.root, nm("W5.TXT"));
    }
    return rc;
}

/* A file already on the medium, unlinked. Its name and its chain were
   durable before the unlink, so the flush `free_chain` begins with is all
   that keeps a crash from leaving the name over a freed chain. The unlink
   above follows its file's own creation, whose size and first cluster are
   still pending in the block the unlink rewrites, and there no order of
   the two can show a name over a freed chain; rmdir flushes before it
   frees. */
static int seq_unlink_synced(void)
{
    void *n = make_file(mnt.root, nm("W7.TXT"));
    int rc;

    if (!n) {
        return -1;
    }
    rc = write_at(n, pattern, 1000, 0);
    if (rc == 0) {
        rc = OPS->fsync(&mnt, n);
    }
    OPS->put(&mnt, n);
    if (rc == 0) {
        rc = OPS->unlink(&mnt, mnt.root, nm("W7.TXT"));
    }
    return rc;
}

/* A file already on the medium, cut to part of its chain. Its size and its
   chain were durable before the truncate, so the flush between the shorter
   size and the cut is all that keeps a crash from leaving the old size over
   a chain that ends early. The truncate above follows its file's own write,
   whose size is still pending in the block the truncate rewrites, and there
   no order of the two can show the old size over the cut chain. */
static int seq_truncate_synced(void)
{
    void *n = make_file(mnt.root, nm("W8.TXT"));
    void *h;
    int rc;

    if (!n) {
        return -1;
    }
    rc = write_at(n, pattern, 2010, 0);
    if (rc == 0) {
        rc = OPS->fsync(&mnt, n);
    }
    if (rc == 0 && OPS->open(&mnt, n, 0, &h) == 0) {
        rc = OPS->truncate(&mnt, h, 700);
        OPS->release(&mnt, h);
    }
    OPS->put(&mnt, n);
    return rc;
}

static int seq_mkdir_rmdir(void)
{
    void *d;
    int rc = OPS->mkdir(&mnt, mnt.root, nm("D1"), 0755);

    if (rc != 0) {
        return rc;
    }
    if (OPS->lookup(&mnt, mnt.root, nm("D1"), &d) != 0) {
        return -1;
    }
    rc = OPS->mkdir(&mnt, d, nm("D2"), 0755);
    if (rc == 0) {
        rc = OPS->rmdir(&mnt, d, nm("D2"));
    }
    OPS->put(&mnt, d);
    if (rc == 0) {
        rc = OPS->rmdir(&mnt, mnt.root, nm("D1"));
    }
    return rc;
}

static int seq_rename(void)
{
    void *n = make_file(mnt.root, nm("R1.TXT"));
    void *boot;
    int rc;

    if (!n) {
        return -1;
    }
    rc = write_at(n, pattern, 700, 0);
    OPS->put(&mnt, n);
    if (rc == 0) {
        rc = OPS->rename(&mnt, mnt.root, nm("R1.TXT"), mnt.root, nm("R2.TXT"));
    }
    if (rc == 0 && OPS->lookup(&mnt, mnt.root, "BOOT", &boot) == 0) {
        rc = OPS->rename(&mnt, mnt.root, nm("R2.TXT"), boot, nm("R3.TXT"));
        if (rc == 0) {
            rc = OPS->mkdir(&mnt, mnt.root, nm("RD"), 0755);
        }
        if (rc == 0) {
            rc = OPS->rename(&mnt, mnt.root, nm("RD"), boot, nm("RD2"));
        }
        OPS->put(&mnt, boot);
    }
    return rc;
}

static int seq_rename_over(void)
{
    void *a = make_file(mnt.root, nm("O1.TXT"));
    void *b = a ? make_file(mnt.root, nm("O2.TXT")) : 0;
    int rc = a && b ? 0 : -1;

    if (rc == 0) {
        rc = write_at(a, "one", 3, 0);
    }
    if (rc == 0) {
        rc = write_at(b, "two", 3, 0);
    }
    if (a) {
        OPS->put(&mnt, a);
    }
    if (b) {
        OPS->put(&mnt, b);
    }
    if (rc == 0) {
        rc = OPS->rename(&mnt, mnt.root, nm("O1.TXT"), mnt.root, nm("O2.TXT"));
    }
    return rc;
}

struct sequence {
    const char *name;
    int (*run)(void);
};

static const struct sequence sequences[] = {
    {"create and write", seq_create_write},
    {"extend across a cluster", seq_extend},
    {"write past the end", seq_past_end},
    {"grow in place", seq_grow_in_place},
    {"truncate", seq_truncate},
    {"unlink", seq_unlink},
    {"mkdir and rmdir", seq_mkdir_rmdir},
    {"rename", seq_rename},
    {"rename over", seq_rename_over},
    {"many files", seq_many_files},
    {"unlink after a sync", seq_unlink_synced},
    {"truncate after a sync", seq_truncate_synced},
};

/* Read the crash state back from the dump: the cache's dirty buffers go
   into the image the dump replaces, so nothing of them survives. */
static void load_dump(void)
{
    void *f;

    stop_after = -1;
    model.stopped = 0;
    bcache_sync(&dev);
    f = fopen(fix_path("fat32_write.img"), "rb");
    if (!f || fread(image, 1, IMG_BYTES, f) != IMG_BYTES) {
        printf("fat32_hosttest: cannot read the dump back\n");
        exit(1);
    }
    fclose(f);
    bcache_invalidate(&dev);
    devmodel_load(&model);
}

/* Make `what` the device's whole state, medium and all, for a mount over
   a crash state. */
static void load_snapshot(const unsigned char *what)
{
    /* What the cache still holds of the stopped run drains into the image
       about to be replaced, so the cache is empty for the snapshot. */
    stop_after = -1;
    model.stopped = 0;
    bcache_sync(&dev);
    if (bcache_invalidate(&dev) != 0) {
        printf("fat32_hosttest: the cache did not empty for a snapshot\n");
        exit(1);
    }
    __builtin_memcpy(image, what, IMG_BYTES);
    devmodel_load(&model);
}

/* The content oracle: after a stop, every file a data sequence writes
   reads, within its published size, either the bytes written to it or
   zeros, never anything else. */
static int oracle_ok(void)
{
    static const char *const names[] = {"W1.TXT", "W2.TXT", "W3.TXT", "W6.TXT", "W4.TXT"};
    int f, i, err;

    for (f = 0; f < 5; f++) {
        void *n;

        if (OPS->lookup(&mnt, mnt.root, nm(names[f]), &n) != 0) {
            n = 0;
            err = -ENOENT;
        }
        int got, zeros = 1, written = 1;

        if (!n) {
            continue;
        }
        got = read_all(n, back, sizeof(back));
        OPS->put(&mnt, n);
        if (got < 0) {
            return 0;
        }
        for (i = 0; i < got; i++) {
            unsigned char want;

            if (f == 0) {
                want = i < 5 ? (unsigned char)"hello"[i] : 0;
            } else if (f == 2) {
                want = i < 10 ? pattern[i] : (i >= 2000 ? pattern[10 + i - 2000] : 0);
            } else if (f == 4 && got == 1500) {
                want = 0; /* regrown from nothing after the truncate to zero */
            } else if (f == 4 && (got == 1000 || got == 2600)) {
                want = i < 700 ? pattern[i] : 0; /* grown from 700 by truncate */
            } else {
                want = pattern[i];
            }
            if (back[i] != want) {
                written = 0;
            }
            if (back[i] != 0) {
                zeros = 0;
            }
        }
        if (!written && !zeros) {
            printf("fat32_hosttest: %s holds bytes nobody wrote (%d bytes)\n", nm(names[f]), got);
            return 0;
        }
    }
    return 1;
}

/* The check's listing of the dumped image, for the long names it shows. */
static int run_check_list(char *out, unsigned long cap)
{
    char cmd[2048], line[600];
    void *f;
    unsigned long n = 0;
    int rc;

    f = fopen(fix_path("fat32_write.img"), "wb");
    if (!f || fwrite(image, 1, IMG_BYTES, f) != IMG_BYTES) {
        exit(1);
    }
    fclose(f);
    fix_format(cmd, sizeof(cmd), "%s/mkfatfixture --check %s/fat32_write.img --list", fix_dir(),
               fix_dir());
    f = popen(cmd, "r");
    if (!f) {
        exit(1);
    }
    out[0] = '\0';
    while (fgets(line, sizeof(line), f) && n + strlen(line) < cap - 1) {
        __builtin_memcpy(out + n, line, strlen(line) + 1);
        n += strlen(line);
    }
    rc = pclose(f);
    return rc == -1 ? -1 : (rc >> 8) & 0xff;
}

/* Long names of every kind, written and read back by name and by alias,
   listed by the check and by mtools; the names refused; the attributes. */
static void long_rows(void)
{
    struct vfs_stat st;
    void *n, *h, *boot;
    int err, i;
    static char name[300];

    fill_pattern();
    load("--junk");
    check(mount_rw("") == 0, "a writable mount for the long-name rows");
    n = make_file(mnt.root, "Long Name One.txt");
    check(n && write_at(n, pattern, 700, 0) == 0, "a long name with spaces and case is created");
    if (n) {
        OPS->put(&mnt, n);
    }
    check(walk("/Long Name One.txt", &err) != 0, "and found by its long name");
    n = walk("/long name one.txt", &err);
    check(n != 0, "with case folded");
    if (n) {
        OPS->put(&mnt, n);
    }
    n = walk("/LONGNA~1.TXT", &err);
    check(n != 0 && read_all(n, back, sizeof(back)) == 700 &&
              __builtin_memcmp(back, pattern, 700) == 0,
          "and by its alias, reading the bytes written");
    if (n) {
        OPS->put(&mnt, n);
    }
    check(make_file(mnt.root, "longname1.txt") != 0 && make_file(mnt.root, "longname2.txt") != 0,
          "two names whose alias base is taken");
    check(walk("/LONGNA~2.TXT", &err) != 0 && walk("/LONGNA~3.TXT", &err) != 0,
          "take the next numbers, as mtools numbered them");
    n = make_file(mnt.root, "Mixed.txt");
    check(n != 0 && walk("/MIXED~1.TXT", &err) != 0, "a name of mixed case takes an alias");
    n = make_file(mnt.root, "a.b.c");
    check(n != 0 && walk("/AB~1.C", &err) != 0, "two dots take an alias");
    n = make_file(mnt.root, "\xc3\xa9t\xc3\xa9.txt");
    check(n != 0 && walk("/\xc3\xa9t\xc3\xa9.txt", &err) != 0 && walk("/_T_~1.TXT", &err) != 0,
          "a name outside ASCII is stored in UTF-16 and read back");
    for (i = 0; i < 200; i++) {
        name[i] = (char)('a' + i % 26);
    }
    name[200] = '\0';
    n = make_file(mnt.root, name);
    check(n != 0 && write_at(n, "long", 4, 0) == 0,
          "a name of two hundred characters, sixteen entries, is created");
    if (n) {
        OPS->put(&mnt, n);
    }
    {
        static char path[300];

        path[0] = '/';
        __builtin_memcpy(path + 1, name, 201);
        n = walk(path, &err);
        check(n != 0 && read_all(n, back, sizeof(back)) == 4, "and read back by it");
        if (!n) {
            printf("fat32_hosttest: the long name's lookup said %d\n", err);
        }
    }
    if (n) {
        OPS->put(&mnt, n);
    }
    check(OPS->create(&mnt, mnt.root, "bad:name.txt", 0644, &n) == -EINVAL &&
              OPS->create(&mnt, mnt.root, "trailing.", 0644, &n) == -EINVAL &&
              OPS->create(&mnt, mnt.root, "trailing ", 0644, &n) == -EINVAL &&
              OPS->create(&mnt, mnt.root, "..", 0644, &n) == -EINVAL,
          "a name FAT cannot store is EINVAL");
    check(OPS->create(&mnt, mnt.root, "twice.txt", 0644, &n) == 0 &&
              OPS->create(&mnt, mnt.root, "twice.txt", 0644, &n) == -EEXIST &&
              OPS->mkdir(&mnt, mnt.root, "twice.txt", 0755) == -EEXIST,
          "a second create or a mkdir of an existing name is EEXIST");
    OPS->put(&mnt, n);
    name[255] = 'a';
    name[256] = '\0';
    for (i = 200; i < 255; i++) {
        name[i] = 'b';
    }
    check(OPS->create(&mnt, mnt.root, name, 0644, &n) == -ENAMETOOLONG,
          "and one past 255 units is ENAMETOOLONG");
    check_consistent("the volume is consistent with the long names on it", 1);
    run_check_list(check_out, sizeof(check_out));
    check(strstr(check_out, "long Long Name One.txt") != 0 &&
              strstr(check_out, "long longname1.txt") != 0 &&
              strstr(check_out, "long \xc3\xa9t\xc3\xa9.txt") != 0,
          "the check lists the long names");

    /* Renames with long names: in place at the same entry count, and moved
       otherwise; the entries of a removed long name are freed. */
    check(OPS->rename(&mnt, mnt.root, "Long Name One.txt", mnt.root, "Long Name Two.txt") == 0 &&
              walk("/Long Name Two.txt", &err) != 0 && walk("/Long Name One.txt", &err) == 0,
          "a long name renamed in place");
    check(OPS->rename(&mnt, mnt.root, "Long Name Two.txt", mnt.root, "SHORT.TXT") == 0 &&
              walk("/SHORT.TXT", &err) != 0,
          "a long name renamed to a short one");
    check(OPS->unlink(&mnt, mnt.root, "longname2.txt") == 0 && walk("/longname2.txt", &err) == 0,
          "a long name unlinked");
    check_consistent("the volume is consistent after the long renames and the unlink", 1);

    /* A rename of an open file: the description writes at the new place. */
    n = walk("/SHORT.TXT", &err);
    boot = walk("/BOOT", &err);
    check(n && boot && OPS->open(&mnt, n, 0, &h) == 0, "an open file to rename");
    check(OPS->rename(&mnt, mnt.root, "SHORT.TXT", boot, "moved name.txt") == 0,
          "renamed across directories while open");
    {
        /* The node is the file's while it is held, under its new name too:
           its advisory locks are found by it (lock.Readme.md). */
        void *renamed = walk("/BOOT/moved name.txt", &err);

        check(renamed && renamed == n, "the new name of a held file reaches the node it had");
        if (renamed) {
            OPS->put(&mnt, renamed);
        }
    }
    check(OPS->write(&mnt, h, pattern, 900, 700) == 900, "and written through its description");
    OPS->release(&mnt, h);
    if (n) {
        OPS->put(&mnt, n);
    }
    if (boot) {
        OPS->put(&mnt, boot);
    }
    n = walk("/BOOT/moved name.txt", &err);
    check(n && OPS->getattr(&mnt, n, &st) == 0 && st.size == 1600,
          "the new name has the size the write gave it");
    if (n) {
        OPS->put(&mnt, n);
    }
    check_consistent("the volume is consistent after the open file's rename", 1);

    /* The attributes a volume can store. */
    n = walk("/BOOT/moved name.txt", &err);
    check(n != 0, "the file for the attribute rows");
    if (n) {
        st.mode = 0555;
        check(OPS->setattr(&mnt, n, VFS_SET_MODE, &st) == 0 && OPS->getattr(&mnt, n, &st) == 0 &&
                  (st.mode & 0777) == 0555,
              "clearing the write bits sets the read-only attribute");
        st.mode = 0755;
        check(OPS->setattr(&mnt, n, VFS_SET_MODE, &st) == 0 && OPS->getattr(&mnt, n, &st) == 0 &&
                  (st.mode & 0777) == 0755,
              "and setting them clears it");
        st.mode = 0700;
        check(OPS->setattr(&mnt, n, VFS_SET_MODE, &st) == -EPERM,
              "a mode change outside the write bits is EPERM");
        st.uid = 5;
        check(OPS->setattr(&mnt, n, VFS_SET_UID, &st) == -EPERM, "an owner change is EPERM");
        st.size = 3;
        check(OPS->setattr(&mnt, n, VFS_SET_SIZE, &st) == -EPERM,
              "a field the call does not store is EPERM, not silence");
        {
            struct vfs_stat before;

            check(OPS->getattr(&mnt, n, &before) == 0 && write_at(n, pattern, 100, 0) == 0 &&
                      OPS->getattr(&mnt, n, &st) == 0 && st.ctime_sec == before.ctime_sec &&
                      st.ctime_nsec == before.ctime_nsec,
                  "a write leaves the creation time, hundredths included, alone");
        }
        st.mtime_sec = 946684800;
        st.atime_sec = 1000000000;
        check(OPS->setattr(&mnt, n, VFS_SET_MTIME | VFS_SET_ATIME, &st) == 0 &&
                  OPS->getattr(&mnt, n, &st) == 0 && st.mtime_sec == 946684800 &&
                  st.atime_sec == 1000000000 - 1000000000 % 86400,
              "the times are stored to FAT's resolution");
        OPS->put(&mnt, n);
    }
    check(OPS->prepare_unmount(&mnt) == 0, "the clean unmount");
    unmount_image();
    check_consistent("the volume is consistent and clean after the long-name rows", 0);
    if (system("which mdir >/dev/null 2>&1") == 0) {
        char cmd[2048];

        fix_format(cmd, sizeof(cmd),
                   "mdir -i %s/fat32_write.img ::/BOOT 2>/dev/null | grep -q 'moved name.txt'",
                   fix_dir());
        check(system(cmd) == 0, "mtools lists the long name this driver wrote");
        fix_format(
            cmd, sizeof(cmd),
            "mdir -i %s/fat32_write.img ::/ 2>/dev/null | grep -q 'LONGNA~2 TXT.*longname1.txt'",
            fix_dir());
        check(system(cmd) == 0, "and the alias this driver gave beside it");
    }
}

/* A dirty volume at mount: clean, repaired, or refused. */
/* The free entries of the active FAT, read from the image: the driver's
 * count after a check must agree with the disk. */
static uint32_t free_in_fat(void)
{
    uint32_t bps = image[11] | ((uint32_t)image[12] << 8);
    uint32_t reserved = image[14] | ((uint32_t)image[15] << 8);
    uint32_t fat = reserved * bps;
    struct vfs_statfs sf;
    uint32_t c;
    uint32_t n = 0;

    if (OPS->statfs(&mnt, &sf) != 0) {
        return 0xffffffffu;
    }
    for (c = 2; c < sf.blocks + 2; c++) {
        uint32_t off = fat + c * 4;
        uint32_t v = image[off] | ((uint32_t)image[off + 1] << 8) |
                     ((uint32_t)image[off + 2] << 16) | ((uint32_t)image[off + 3] << 24);

        if ((v & 0x0fffffffu) == 0) {
            n++;
        }
    }
    return n;
}

/* The directory tree under rename: not into itself, and a held directory
   follows its move. */
static void tree_rows(void)
{
    void *a = 0, *b = 0, *c = 0;
    struct vfs_stat st;

    load("--junk");
    check(mount_rw("") == 0, "a writable mount for the tree rows");
    check(OPS->mkdir(&mnt, mnt.root, "A", 0755) == 0 && OPS->lookup(&mnt, mnt.root, "A", &a) == 0 &&
              OPS->mkdir(&mnt, a, "B", 0755) == 0 && OPS->lookup(&mnt, a, "B", &b) == 0,
          "a tree A/B");
    check(OPS->rename(&mnt, mnt.root, "A", b, "X") == -EINVAL,
          "a directory renamed under itself is EINVAL");
    check(OPS->rename(&mnt, mnt.root, "A", a, "X") == -EINVAL, "and into itself");
    check(OPS->rename(&mnt, a, "B", mnt.root, "C") == 0, "a held directory is renamed away");
    st.mode = 0555;
    check(OPS->setattr(&mnt, b, VFS_SET_MODE, &st) == 0, "and set through its held node");
    if (b) {
        OPS->put(&mnt, b);
    }
    if (a) {
        OPS->put(&mnt, a);
    }
    check(OPS->prepare_unmount(&mnt) == 0, "its clean unmount");
    unmount_image();
    check_consistent("and the tree walks clean", 0);
    /* Read back from the disk, not from the node the rows held. */
    check(mount_image("") == 0 && OPS->lookup(&mnt, mnt.root, "C", &c) == 0 &&
              OPS->getattr(&mnt, c, &st) == 0 && (st.mode & 0222) == 0,
          "which wrote the entry where it now is");
    if (c) {
        OPS->put(&mnt, c);
    }
    unmount_image();
}

/* ---- two mounts at once ---- */

/* A second volume on a device of its own, read and written in place. The
   first write after `interleave` is set stands for a write in flight, during
   which the kernel may run another process: the same operation on the first
   mount by `proc_b` runs inside it, under that mount's namespace lock. */
static process_t proc_b;
static unsigned char image_b[IMG_BYTES];
static struct vfs_mount mnt_b;
enum { NOTHING, CREATE, MKDIR, RENAME };
static int interleave; /* the operation to run inside the next write */
static int interleaved;
static char first_name[64];

static int img_b_read(block_device_t *d, uint64_t lba, uint32_t count, void *buf)
{
    (void)d;
    __builtin_memcpy(buf, image_b + lba * 512, (unsigned long)count * 512);
    return 0;
}

static int img_b_write(block_device_t *d, uint64_t lba, uint32_t count, const void *buf)
{
    void *n = 0;
    int op = interleave, rc;

    (void)d;
    if (op != NOTHING) {
        interleave = NOTHING;
        snprintf(first_name, sizeof(first_name), "Made On The First Mount %d.txt", interleaved + 1);
        cpu_table[0].current = &proc_b;
        vfs_ns_lock(&mnt);
        if (op == CREATE) {
            rc = OPS->create(&mnt, mnt.root, first_name, 0644, &n);
            if (rc == 0) {
                OPS->put(&mnt, n);
            }
        } else if (op == MKDIR) {
            rc = OPS->mkdir(&mnt, mnt.root, first_name, 0755);
        } else {
            rc = OPS->rename(&mnt, mnt.root, "plain.txt", mnt.root, first_name);
        }
        if (rc == 0) {
            interleaved++;
        }
        vfs_ns_unlock(&mnt);
        cpu_table[0].current = &proc_a;
    }
    __builtin_memcpy(image_b + lba * 512, buf, (unsigned long)count * 512);
    return 0;
}

static block_device_t dev_b = {512, 8192u, BLOCK_FAULT_SAFE, 0, 0, 0, img_b_read, img_b_write, 0,
                               0,   0};

/* Whether `name` is in the root of `m`. */
static int in_root(struct vfs_mount *m, const char *name)
{
    void *n;

    if (OPS->lookup(m, m->root, name, &n) != 0) {
        return 0;
    }
    OPS->put(m, n);
    return 1;
}

/* A create, a mkdir and a rename on the second mount, each with the same
   operation on the first inside its first device write, after the new name
   is prepared and before it is written. Each mount must write the name it
   was given. */
static void interleaved_rows(void)
{
    void *n = 0;
    int before;

    load("--junk");
    __builtin_memcpy(image_b, image, IMG_BYTES);
    proc_b.pid = 2;
    check(mount_rw("") == 0, "a writable first mount for the interleaved rows");
    vfs_ns_unlock(&mnt);
    __builtin_memset(&mnt_b, 0, sizeof(mnt_b));
    mnt_b.type = &vfs_vfat_type;
    mnt_b.dev = &dev_b;
    mnt_b.ns_holder = -1;
    check(vfs_vfat_type.mount(&mnt_b, &dev_b, 0, "") == 0, "and a second on a device of its own");
    vfs_ns_lock(&mnt_b);
    interleaved = 0;

    interleave = CREATE;
    check(OPS->create(&mnt_b, mnt_b.root, "Created On The Second Mount.txt", 0644, &n) == 0 &&
              interleaved == 1,
          "a create on the second mount with one on the first inside it");
    if (n) {
        OPS->put(&mnt_b, n);
    }
    check(in_root(&mnt_b, "Created On The Second Mount.txt") && !in_root(&mnt_b, first_name) &&
              in_root(&mnt, first_name),
          "the create writes its own name, and the first mount its own");

    interleave = MKDIR;
    check(OPS->mkdir(&mnt_b, mnt_b.root, "Directory On The Second Mount", 0755) == 0 &&
              interleaved == 2,
          "a mkdir with one inside it");
    check(in_root(&mnt_b, "Directory On The Second Mount") && !in_root(&mnt_b, first_name) &&
              in_root(&mnt, first_name),
          "the mkdir writes its own name");

    n = 0;
    check(OPS->create(&mnt_b, mnt_b.root, "plain.txt", 0644, &n) == 0, "a short name to rename");
    if (n) {
        OPS->put(&mnt_b, n);
    }
    n = 0;
    vfs_ns_lock(&mnt);
    check(OPS->create(&mnt, mnt.root, "plain.txt", 0644, &n) == 0, "and one on the first mount");
    if (n) {
        OPS->put(&mnt, n);
    }
    vfs_ns_unlock(&mnt);
    before = interleaved;
    interleave = RENAME;
    check(OPS->rename(&mnt_b, mnt_b.root, "plain.txt", mnt_b.root,
                      "Renamed On The Second Mount.txt") == 0 &&
              interleaved == before + 1,
          "a rename with one inside it");
    check(in_root(&mnt_b, "Renamed On The Second Mount.txt") && !in_root(&mnt_b, "plain.txt") &&
              !in_root(&mnt_b, first_name) && in_root(&mnt, first_name),
          "the rename writes its own name");

    interleave = NOTHING;
    check(OPS->prepare_unmount(&mnt_b) == 0, "the second mount's clean unmount");
    OPS->put(&mnt_b, mnt_b.root);
    OPS->unmount(&mnt_b);
    bcache_sync(&dev_b);
    check(bcache_invalidate(&dev_b) == 0, "and the cache keeps none of its blocks");
    vfs_ns_lock(&mnt);
    check(OPS->prepare_unmount(&mnt) == 0, "the first mount's clean unmount");
    unmount_image();
    check_consistent("and the first volume walks clean", 0);
}

static void dirty_rows(void)
{
    void *n;
    int err;

    load("--dirty");
    check(mount_rw("") == 0 && !(mnt.flags & VFS_MS_RDONLY),
          "a dirty but consistent volume mounts writable");
    check(OPS->prepare_unmount(&mnt) == 0 && (image[65] & 1) == 0,
          "and its clean unmount clears the flag");
    unmount_image();
    check_consistent("after which the walk calls it clean", 0);

    load("--dirty-surplus");
    check(mount_rw("") == 0 && !(mnt.flags & VFS_MS_RDONLY),
          "a surplus tail is repaired and the volume mounts writable");
    {
        struct vfs_statfs sf;

        check(OPS->statfs(&mnt, &sf) == 0 && sf.bfree == free_in_fat(),
              "the free count after the repair is the FAT's, freed tail included");
    }
    n = walk("/BOOT/GRUB/GRUB.CFG", &err);
    check(n && read_all(n, back, sizeof(back)) == 40 && back[0] == 'x',
          "the file the tail hung off reads");
    if (n) {
        OPS->put(&mnt, n);
    }
    check(OPS->prepare_unmount(&mnt) == 0, "its clean unmount");
    unmount_image();
    check_consistent("and the walk finds no surplus and no lost cluster", 0);

    load("--dirty-orphan");
    check(mount_rw("") == 0 && !(mnt.flags & VFS_MS_RDONLY),
          "orphan long-name entries are freed and the volume mounts writable");
    check(OPS->prepare_unmount(&mnt) == 0, "its clean unmount");
    unmount_image();
    check_consistent("and the walk finds no orphan", 0);

    load("--dirty-mirror");
    check(mount_rw("") == 0 && !(mnt.flags & VFS_MS_RDONLY),
          "a lagging mirror is rewritten and the volume mounts writable");
    check(OPS->prepare_unmount(&mnt) == 0, "its clean unmount");
    unmount_image();
    check_consistent("and the mirrors agree", 0);

    load("--dirty-cross");
    __builtin_memcpy(image_before, image, sizeof(image));
    check(mount_rw("") == 0 && (mnt.flags & VFS_MS_RDONLY),
          "a cross-linked volume mounts read-only");
    check(__builtin_memcmp(image_before, image, sizeof(image)) == 0, "with the image untouched");
    n = walk("/BOOT/GRUB/GRUB.CFG", &err);
    check(n && read_all(n, back, sizeof(back)) == 40, "and still reads");
    if (n) {
        OPS->put(&mnt, n);
    }
    check(OPS->create(&mnt, mnt.root, "NO.TXT", 0644, &n) == -EROFS, "and refuses a write");
    unmount_image();

    load("--dirty-surplus-cross");
    __builtin_memcpy(image_before, image, sizeof(image));
    check(mount_rw("") == 0 && (mnt.flags & VFS_MS_RDONLY),
          "a surplus tail that is another file's chain is a cross-link, not a repair");
    check(__builtin_memcmp(image_before, image, sizeof(image)) == 0, "and the image is untouched");
    n = walk("/BOOT/GRUB/GRUB.CFG", &err);
    check(n && read_all(n, back, sizeof(back)) == 40 && back[0] == 'x',
          "the file the tail ran into still reads");
    if (n) {
        OPS->put(&mnt, n);
    }
    unmount_image();
    check((image[65] & 1) == 1, "with its dirty flag left set");

    load("--dirty-empty-chain");
    __builtin_memcpy(image_before, image, sizeof(image));
    check(mount_rw("") == 0 && (mnt.flags & VFS_MS_RDONLY),
          "a file of size zero with a cluster mounts read-only");
    check(__builtin_memcmp(image_before, image, sizeof(image)) == 0,
          "and its chain is not freed under the entry");
    unmount_image();

    /* FSInfo's count is not trusted: the driver counts the FAT. */
    load("--fsinfo-wrong");
    {
        struct vfs_statfs sf;

        check(mount_rw("") == 0 && OPS->statfs(&mnt, &sf) == 0 && sf.bfree != 12345,
              "a wrong FSInfo count is not what statfs reports");
        check(OPS->prepare_unmount(&mnt) == 0, "the clean unmount rewrites FSInfo");
        unmount_image();
        check_consistent("and the walk finds the count right", 0);
    }
}

/* What the two sequences on a file its `fsync` put on the medium left: the
   unlinked file is gone, and the truncated one reads its first 700 bytes. */
static void synced_rows_left(void)
{
    struct vfs_stat st;
    void *n;
    int err;

    check(walk("/W7.TXT", &err) == 0 && err == -ENOENT, "the file unlinked after a sync is gone");
    n = walk("/W8.TXT", &err);
    check(n && OPS->getattr(&mnt, n, &st) == 0 && st.size == 700 &&
              read_all(n, back, sizeof(back)) == 700 && __builtin_memcmp(back, pattern, 700) == 0,
          "the file truncated after a sync reads back its first 700 bytes");
    if (n) {
        OPS->put(&mnt, n);
    }
}

static void write_rows(void)
{
    struct vfs_stat st;
    struct vfs_statfs sf, sf2;
    void *n, *h, *d, *up, *boot;
    int err, i, ok;
    static unsigned char big[512 * 1024];

    fill_pattern();
    load("--junk");
    check(mount_rw("") == 0, "a writable mount of the FSInfo volume");
    check(image[65] == 0, "the boot-sector dirty flag is clear before the first write");

    /* Each sequence in turn, the volume consistent after every one. */
    for (i = 0; i < (int)(sizeof(sequences) / sizeof(sequences[0])); i++) {
        char what[120];

        snprintf(what, sizeof(what), "the sequence '%s' runs", sequences[i].name);
        check(sequences[i].run() == 0, what);
        snprintf(what, sizeof(what), "the volume is consistent after '%s'", sequences[i].name);
        check_consistent(what, 1);
    }
    check((image[65] & 1) == 1, "the boot-sector dirty flag is set by the first write");

    /* What the sequences left. */
    n = walk("/W1.TXT", &err);
    check(n && read_all(n, back, sizeof(back)) == 5 && __builtin_memcmp(back, "hello", 5) == 0,
          "the created file reads back");
    if (n) {
        OPS->put(&mnt, n);
    }
    n = walk("/W2.TXT", &err);
    check(n && read_all(n, back, sizeof(back)) == 1200 &&
              __builtin_memcmp(back, pattern, 1200) == 0,
          "the file extended across a cluster reads back");
    if (n) {
        OPS->put(&mnt, n);
    }
    check(walk("/R1.TXT", &err) == 0 && walk("/R2.TXT", &err) == 0,
          "the renamed file is gone from its old names");
    n = walk("/W3.TXT", &err);
    ok = n && read_all(n, back, sizeof(back)) == 2010 && __builtin_memcmp(back, pattern, 10) == 0 &&
         __builtin_memcmp(back + 2000, pattern + 10, 10) == 0;
    for (i = 10; ok && i < 2000; i++) {
        if (back[i] != 0) {
            ok = 0;
        }
    }
    check(ok, "a write past the end reads zeros between");
    if (n) {
        OPS->put(&mnt, n);
    }
    n = walk("/W4.TXT", &err);
    ok = n && OPS->getattr(&mnt, n, &st) == 0 && st.size == 1500 &&
         read_all(n, back, sizeof(back)) == 1500;
    for (i = 0; ok && i < 1500; i++) {
        if (back[i] != 0) {
            ok = 0;
        }
    }
    check(ok, "a file truncated to zero and grown reads zeros");
    if (n) {
        OPS->put(&mnt, n);
    }
    check(walk("/W5.TXT", &err) == 0 && err == -ENOENT, "the unlinked file is gone");
    n = walk("/W6.TXT", &err);
    check(n && read_all(n, back, sizeof(back)) == 200 && __builtin_memcmp(back, pattern, 200) == 0,
          "the file grown in place reads back");
    if (n) {
        OPS->put(&mnt, n);
    }
    check(walk("/D1", &err) == 0 && err == -ENOENT, "the removed directory is gone");
    synced_rows_left();
    {
        int seen, count;

        d = walk("/MANY", &err);
        ino_count = 0;
        count = d ? list_dir(d, "F19", &seen) : -1;
        check(count == 22 && seen, "the directory grown past its cluster lists its twenty files");
        if (d) {
            OPS->put(&mnt, d);
        }
    }
    n = walk("/BOOT/R3.TXT", &err);
    check(n && read_all(n, back, sizeof(back)) == 700 && __builtin_memcmp(back, pattern, 700) == 0,
          "the file renamed across directories reads back");
    if (n) {
        OPS->put(&mnt, n);
    }
    d = walk("/BOOT/RD2", &err);
    boot = walk("/BOOT", &err);
    check(d && boot && OPS->lookup(&mnt, d, "..", &up) == 0 && up == boot,
          "the moved directory's .. is its new parent");
    if (d && boot) {
        OPS->put(&mnt, up);
    }
    if (d) {
        OPS->put(&mnt, d);
    }
    if (boot) {
        OPS->put(&mnt, boot);
    }
    n = walk("/O2.TXT", &err);
    check(n && read_all(n, back, sizeof(back)) == 3 && __builtin_memcmp(back, "one", 3) == 0,
          "a rename over a file replaces it");
    if (n) {
        OPS->put(&mnt, n);
    }
    check(walk("/O1.TXT", &err) == 0, "and the source name is gone");

    /* The open-file cases. */
    n = make_file(mnt.root, "OPEN.TXT");
    check(n && write_at(n, "open", 4, 0) == 0 && OPS->open(&mnt, n, 0, &h) == 0, "an open file");
    check(OPS->statfs(&mnt, &sf) == 0, "the free count before an unlink");
    check(OPS->unlink(&mnt, mnt.root, "OPEN.TXT") == 0, "unlink of the open file");
    check(walk("/OPEN.TXT", &err) == 0, "which no lookup finds");
    check(OPS->read(&mnt, h, back, 4, 0) == 4 && __builtin_memcmp(back, "open", 4) == 0,
          "while the description still reads it");
    check(OPS->write(&mnt, h, "more", 4, 4) == 4 && OPS->read(&mnt, h, back, 8, 0) == 8,
          "and writes it");
    check(OPS->statfs(&mnt, &sf2) == 0 && sf2.bfree == sf.bfree,
          "its clusters stay until the last close");
    OPS->release(&mnt, h);
    OPS->put(&mnt, n);
    check(OPS->statfs(&mnt, &sf2) == 0 && sf2.bfree == sf.bfree + 1, "and go at the last close");
    check_consistent("the volume is consistent after the unlinked file's last close", 1);

    n = walk("/O2.TXT", &err);
    check(n && OPS->open(&mnt, n, 0, &h) == 0, "a description on a file about to be replaced");
    d = make_file(mnt.root, "O3.TXT");
    check(d && write_at(d, "three", 5, 0) == 0, "the replacing file");
    if (d) {
        OPS->put(&mnt, d);
    }
    check(OPS->rename(&mnt, mnt.root, "O3.TXT", mnt.root, "O2.TXT") == 0,
          "rename over the open file");
    check(OPS->read(&mnt, h, back, 3, 0) == 3 && __builtin_memcmp(back, "one", 3) == 0,
          "the old description keeps the file it opened");
    OPS->release(&mnt, h);
    OPS->put(&mnt, n);
    n = walk("/O2.TXT", &err);
    check(n && read_all(n, back, sizeof(back)) == 5 && __builtin_memcmp(back, "three", 5) == 0,
          "and the name reads the new file");
    if (n) {
        OPS->put(&mnt, n);
    }
    check_consistent("the volume is consistent after the replacing rename", 1);

    check(OPS->mkdir(&mnt, mnt.root, "HELD", 0755) == 0, "a directory to hold");
    d = walk("/HELD", &err);
    check(d && OPS->rmdir(&mnt, mnt.root, "HELD") == -EBUSY, "rmdir of a held directory is EBUSY");
    if (d) {
        OPS->put(&mnt, d);
    }
    check(OPS->rmdir(&mnt, mnt.root, "HELD") == 0, "and succeeds once it is put");
    check(OPS->rmdir(&mnt, mnt.root, "BOOT") == -ENOTEMPTY,
          "rmdir of a full directory is ENOTEMPTY");

    /* ENOSPC is decided before anything is written. */
    n = make_file(mnt.root, "BIG.BIN");
    check(OPS->statfs(&mnt, &sf) == 0, "the free count before a full write");
    check(n && write_at(n, big, sizeof(big), 0) == 0, "half a megabyte is written");
    {
        int before = writes_made;

        check(n && write_at(n, big, sizeof(big), 8 * 1024 * 1024) == -ENOSPC &&
                  writes_made == before,
              "a write past the volume's room is ENOSPC, with no sector written");
    }
    ok = n && OPS->getattr(&mnt, n, &st) == 0 && st.size == sizeof(big);
    check(ok, "and the file keeps the size it had");
    if (n) {
        OPS->put(&mnt, n);
    }
    check(OPS->statfs(&mnt, &sf2) == 0 && sf2.bfree + sizeof(big) / 512 == sf.bfree,
          "the free count is what the successful write took");
    check_consistent("the volume is consistent after ENOSPC", 1);

    /* The clean unmount clears the flags and writes FSInfo, and mtools
       reads what was written. */
    check(OPS->prepare_unmount(&mnt) == 0, "prepare_unmount");
    check((image[65] & 1) == 0, "the boot-sector dirty flag is clear after it");
    unmount_image();
    check_consistent("the volume is consistent and clean after the unmount", 0);
    if (system("which mcopy >/dev/null 2>&1") == 0) {
        char cmd[2048];
        void *f;
        unsigned long got;

        fix_format(
            cmd, sizeof(cmd),
            "mcopy -n -i %s/fat32_write.img ::/BOOT/R3.TXT %s/fat32_mtools.out >/dev/null 2>&1",
            fix_dir(), fix_dir());
        check(system(cmd) == 0, "mtools copies a file this driver wrote");
        f = fopen(fix_path("fat32_mtools.out"), "rb");
        got = f ? fread(back, 1, sizeof(back), f) : 0;
        if (f) {
            fclose(f);
        }
        check(got == 700 && __builtin_memcmp(back, pattern, 700) == 0,
              "with the bytes this driver wrote");
        fix_format(cmd, sizeof(cmd), "mdir -i %s/fat32_write.img ::/BOOT 2>/dev/null | grep -q RD2",
                   fix_dir());
        check(system(cmd) == 0, "and lists the directory this driver moved");
    } else {
        printf("fat32_hosttest: mtools absent, its comparison skipped\n");
    }
}

/* The interruption matrix: every sequence stopped after each of its
   writes, and at each stop the states the medium can be in, with none,
   the newest one, two, and so on, or all of the writes pending since the
   driver's last barrier, each judged by the walk and, for the data
   sequences, by the content oracle. A barrier the driver leaves out widens
   what is pending at the stops after it, and one of the states then shows
   the write that should have waited. Then each barrier of the sequence
   made to fail, the driver's answer whatever it is, and the medium judged
   with half of what was pending at it; and the machine stopped at each of
   the four writes after the failure, the states judged again, since what
   the driver does to undo a failed step has an order of its own. */
static const char *keep_name(int keep, int pending)
{
    static char text[40];

    if (keep == 0) {
        return "none";
    }
    if (keep >= pending) {
        return "all";
    }
    snprintf(text, sizeof(text), "the newest %d", keep);
    return text;
}

/* One stop's states, judged: the walk of each, and the oracle where the
   sequence has one. */
static void judge_stop(int i, const char *stage)
{
    int keeps, keep;

    check(!model.overflow, "the model held every pending write");
    keeps = model.npending < DEVMODEL_KEEP_MOST ? model.npending : DEVMODEL_KEEP_MOST;
    /* Every state is taken before any is loaded, since loading one settles
       the model's pending writes. */
    for (keep = 0; keep <= keeps; keep++) {
        devmodel_snapshot(&model, keep, snaps[keep]);
    }
    if (keeps < model.npending) {
        devmodel_snapshot(&model, model.npending, snaps[keeps + 1]);
        keeps++;
    }
    for (keep = 0; keep <= keeps; keep++) {
        char what[240];
        const char *name = keep_name(keep, keeps);

        if (run_check_on(snaps[keep], check_out, sizeof(check_out)) < 0 || check_out[0] == '\0') {
            check(0, "the check ran over the state and said something");
        }
        load_snapshot(snaps[keep]);
        if (i < 5) {
            /* The content oracle over the crash state, read-only. */
            snprintf(what, sizeof(what),
                     "'%s' %s, %s of the pending writes kept, reads only "
                     "what was written",
                     sequences[i].name, stage, name);
            if (mount_image("") == 0) {
                check(oracle_ok(), what);
                unmount_image();
            } else {
                check(0, what);
            }
        }
        snprintf(what, sizeof(what),
                 "'%s' %s, %s of the pending writes kept, leaves a consistent volume",
                 sequences[i].name, stage, name);
        check(findings_allowed(check_out), what);
        if (!findings_allowed(check_out)) {
            printf("fat32_hosttest: the check said:\n%s", check_out);
        }
    }
}

static void matrix_pass(void)
{
    int i, k, total = 0;

    for (i = 0; i < (int)(sizeof(sequences) / sizeof(sequences[0])); i++) {
        int n, barriers, b;

        load("--junk");
        stop_after = -1;
        writes_made = 0;
        if (mount_rw("") != 0) {
            check(0, "the matrix's volume mounts");
            return;
        }
        sequences[i].run();
        n = writes_made;
        OPS->put(&mnt, mnt.root);
        OPS->unmount(&mnt);
        barriers = model.barriers;
        for (k = 1; k <= n; k++) {
            char stage[60];

            load("--junk");
            writes_made = 0;
            stop_after = k;
            if (mount_rw("") != 0) {
                check(0, "the matrix's volume mounts again");
                stop_after = -1;
                return;
            }
            sequences[i].run();
            OPS->put(&mnt, mnt.root);
            OPS->unmount(&mnt);
            snprintf(stage, sizeof(stage), "stopped after write %d of %d", k, n);
            judge_stop(i, stage);
            total++;
        }
        for (b = 1; b <= barriers; b++) {
            char what[200];
            int failed_at, j;

            load("--junk");
            writes_made = 0;
            stop_after = -1;
            model.fail_barrier = b;
            if (mount_rw("") != 0) {
                check(0, "the matrix's volume mounts for a failing barrier");
                model.fail_barrier = 0;
                return;
            }
            sequences[i].run();
            OPS->put(&mnt, mnt.root);
            OPS->unmount(&mnt);
            model.fail_barrier = 0;
            check(model.failed, "the barrier failed as asked");
            failed_at = model.failed_at;
            devmodel_snapshot(&model, DEVMODEL_PENDING_MAX, snap);
            run_check_on(snap, check_out, sizeof(check_out));
            snprintf(what, sizeof(what),
                     "'%s' with barrier %d of %d failed leaves a consistent volume",
                     sequences[i].name, b, barriers);
            check(findings_allowed(check_out), what);
            if (!findings_allowed(check_out)) {
                printf("fat32_hosttest: the check said:\n%s", check_out);
            }
            total++;
            for (j = 0; j <= 3; j++) {
                load("--junk");
                writes_made = 0;
                stop_after = failed_at + j;
                model.fail_barrier = b;
                if (mount_rw("") != 0) {
                    check(0, "the matrix's volume mounts for a failing barrier and a stop");
                    model.fail_barrier = 0;
                    stop_after = -1;
                    return;
                }
                sequences[i].run();
                OPS->put(&mnt, mnt.root);
                OPS->unmount(&mnt);
                model.fail_barrier = 0;
                snprintf(what, sizeof(what),
                         "with barrier %d of %d failed and stopped %d writes after", b, barriers,
                         j);
                judge_stop(i, what);
                total++;
            }
        }
    }
    printf("fat32_hosttest: %d stops walked with %s names\n", total, long_names ? "long" : "short");
}

/* What a sync promises: a file written and then synced, through `fsync`,
   the mount's `sync` or the unmount, is on the medium the moment the call
   returns, so a crash right after, one that keeps nothing of what was
   still pending, keeps the file. A sync that does not reach the medium
   passes every other row, since the cache answers the reads. */
static void durability_row(int how, const char *what)
{
    void *n;
    int err, rc = -1;

    load("--junk");
    if (mount_rw("") != 0) {
        check(0, "the durability row's volume mounts");
        return;
    }
    n = make_file(mnt.root, nm("D1.TXT"));
    if (n && write_at(n, pattern, 3000, 0) == 0) {
        if (how == 0) {
            rc = OPS->fsync(&mnt, n);
        } else if (how == 1) {
            rc = OPS->sync(&mnt);
        }
        OPS->put(&mnt, n);
        if (how == 2) {
            rc = OPS->prepare_unmount(&mnt);
        }
    } else if (n) {
        OPS->put(&mnt, n);
    }
    check(rc == 0, what);
    OPS->put(&mnt, mnt.root);
    OPS->unmount(&mnt);
    devmodel_snapshot(&model, DEVMODEL_KEEP_NONE, snap);
    load_snapshot(snap);
    if (mount_image("") != 0) {
        check(0, "the medium after the sync mounts");
        return;
    }
    n = walk(nm("/D1.TXT"), &err);
    check(n != 0 && read_all(n, back, sizeof(back)) == 3000 &&
              __builtin_memcmp(back, pattern, 3000) == 0,
          "and the file is on the medium with its bytes, nothing pending kept");
    if (n) {
        OPS->put(&mnt, n);
    }
    unmount_image();
}

static void durability_rows(void)
{
    fill_pattern();
    durability_row(0, "a file is written and fsynced");
    durability_row(1, "a file is written and the volume synced");
    durability_row(2, "a file is written and the volume unmounted");
}

static void matrix_rows(void)
{
    fill_pattern();
    long_names = 0;
    matrix_pass();
    long_names = 1;
    matrix_pass();
    long_names = 0;
}

static void node_rows(void)
{
    void *held[40];
    int i, err, took = 0;

    load("--vfs");
    err = mount_image("");
    if (err != 0) {
        printf("fat32_hosttest: the node row's mount said %d\n", err);
    }
    check(err == 0, "the volume mounts for the node row");
    /* All forty files of /MANY held at once, with the root and the
       directory, past the thirty-two nodes the pool once had. */
    {
        void *many = walk("/many", &err);
        char name[8];

        check(many != 0, "the directory of forty files is found");
        for (i = 0; many && i < 40; i++) {
            name[0] = 'M';
            name[1] = (char)('0' + i / 10);
            name[2] = (char)('0' + i % 10);
            name[3] = '\0';
            err = OPS->lookup(&mnt, many, name, &held[took]);
            if (err == 0) {
                took++;
            }
        }
        check(took == 40, "forty files are held at once");
        {
            void *again;

            check(OPS->lookup(&mnt, many, "M05", &again) == 0 && again == held[5],
                  "a held name looked up again is the held node");
            if (again) {
                OPS->put(&mnt, again);
            }
        }
        for (i = 0; i < took; i++) {
            OPS->put(&mnt, held[i]);
        }
        if (many) {
            OPS->put(&mnt, many);
        }
    }
    unmount_image();
}

static int ns_locked;

/* The lock as the kernel keeps it: re-entrant by holder, and what the
   driver's assertion at every mutation reads. The rows call the namespace
   operations the way the common layer does, with the lock held. */
void vfs_ns_lock(struct vfs_mount *mnt)
{
    mnt->ns_holder = (int)current_proc->pid;
    mnt->ns_depth++;
    ns_locked++;
}

void vfs_ns_unlock(struct vfs_mount *mnt)
{
    if (--mnt->ns_depth == 0) {
        mnt->ns_holder = -1;
    }
    ns_locked--;
}

void rtc_realtime(int64_t *seconds, int64_t *nanoseconds)
{
    static int calls;

    *seconds = 1583323200; /* 2020-03-04 12:00:00 */
    /* The hundredths move from call to call, so a stamp that touches the
       creation hundredths is seen. */
    *nanoseconds = (int64_t)(++calls % 100) * 10000000;
}

int vfs_ns_held_by_caller(void)
{
    return 0;
}

void vfs_device_failed(block_device_t *dev, uint64_t lba, int rc)
{
    (void)dev;
    (void)lba;
    (void)rc;
}

int wq_wait_event_nointr(wait_queue_t *wq, wq_cond_fn cond, void *arg)
{
    return wq_wait_event(wq, cond, arg);
}

int main(int argc, char **argv)
{
    proc_a.pid = 1;
    devmodel_init(&model, medium, image, IMG_BYTES);
    if (argc > 1) {
        /* The mode tests/host/fat_barriers_mutate.sh runs: the rows that
           judge the driver's barriers and no other, the first failure
           ending the run. */
        if (!streq(argv[1], "--barriers")) {
            printf("fat32_hosttest: usage: fat32_hosttest [--barriers]\n");
            return 2;
        }
        first_failure_ends = 1;
        matrix_rows();
        durability_rows();
        printf("fat32_hosttest: OK\n");
        fix_passed();
        return 0;
    }
    well_formed_rows();
    small_and_active_rows();
    malformed_rows();
    node_rows();
    write_rows();
    long_rows();
    tree_rows();
    interleaved_rows();
    dirty_rows();
    matrix_rows();
    durability_rows();
    if (fails == 0) {
        printf("fat32_hosttest: OK\n");
        fix_passed();
        return 0;
    }
    /* Not the count: an exit status keeps its low eight bits, and 256
       failures would read as a pass. */
    printf("fat32_hosttest: %d failure(s)\n", fails);
    return 1;
}
