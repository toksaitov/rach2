/*
 * fat32_hosttest.c: the vfat driver over the buffer cache over an image.
 *
 * host-deps: rach2/sys/fs/fat32.c rach2/sys/fs/bcache.c
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 *
 * The volumes are mkfatfixture's, built here on first use the way the
 * bootctl test builds them. The well-formed volume (`--vfs`) must read back
 * exactly what the builder wrote: the long names, the one across a 4 KiB
 * boundary, the surrogate pair, the bad checksum falling back to a
 * lower-cased short name, the 0x05 short name reachable by its long name,
 * the fragmented file, the empty file, `..` back to the root, and the
 * bytes of every file. The malformed volumes are refused at mount or fail
 * at the file they damage, under AddressSanitizer. No hosted headers: the
 * kernel headers define the fixed-width types.
 */

#include "../../rach2/sys/fs/fat32.h"
#include "../../rach2/sys/fs/bcache.h"
#include "../../rach2/sys/lib/kerrno.h"
#include "../../rach2/sys/proc/process.h"

extern int printf(const char *fmt, ...);
extern int snprintf(char *s, unsigned long n, const char *fmt, ...);
extern int system(const char *cmd);
extern int access(const char *path, int mode);
extern void exit(int status);
extern void *fopen(const char *path, const char *mode);
extern unsigned long fread(void *buf, unsigned long size, unsigned long n, void *f);
extern int fclose(void *f);

#define FIXDIR    "/tmp/rach2_verify"
#define IMG_BYTES (8192u * 512u)

/* ---- stubs for what the driver and the cache reach outside themselves ---- */

process_t proc_a;
process_t *current_proc = &proc_a;

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

static unsigned char image[IMG_BYTES];
static int reads_made;

static int img_read(block_device_t *d, uint64_t lba, uint32_t count, void *buf)
{
    (void)d;
    reads_made++;
    if (lba + count > 8192u) {
        return -5;
    }
    __builtin_memcpy(buf, image + lba * 512, count * 512);
    return 0;
}

static int img_write(block_device_t *d, uint64_t lba, uint32_t count, const void *buf)
{
    (void)d;
    (void)lba;
    (void)count;
    (void)buf;
    return -5; /* the driver must never write */
}

static block_device_t dev = {512, 8192u, BLOCK_FAULT_SAFE, 0, img_read, img_write, 0, 0};

int block_request(block_device_t *device, int write, uint64_t lba, uint32_t count, void *data)
{
    if (write) {
        return device->write(device, lba, count, data);
    }
    return device->read(device, lba, count, data);
}

/* ---- the rows ---- */

static int fails;

static void check(int ok, const char *what)
{
    if (!ok) {
        fails++;
        printf("FAIL: %s\n", what);
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

/** Build the named fixture and load it into the image. */
static void load(const char *opt)
{
    char cmd[256];
    void *f;

    /* Built on every run, so an edited builder is the one that runs. */
    snprintf(cmd, sizeof(cmd), "cc -O2 -o %s/mkfatfixture tests/host/mkfatfixture.c", FIXDIR);
    if (system(cmd) != 0) {
        printf("fat32_hosttest: cannot build the fixture builder\n");
        exit(1);
    }
    snprintf(cmd, sizeof(cmd), "%s/mkfatfixture %s/fat32_case.img %s", FIXDIR, FIXDIR, opt);
    if (system(cmd) != 0) {
        printf("fat32_hosttest: fixture builder failed for %s\n", opt);
        exit(1);
    }
    f = fopen(FIXDIR "/fat32_case.img", "rb");
    if (!f || fread(image, 1, IMG_BYTES, f) != IMG_BYTES) {
        printf("fat32_hosttest: fixture did not load\n");
        exit(1);
    }
    fclose(f);
    bcache_invalidate(&dev);
}

static struct vfs_mount mnt;

/* Mount the loaded image read-only with `options`; the constructor's rc. */
static int mount_image(const char *options)
{
    __builtin_memset(&mnt, 0, sizeof(mnt));
    mnt.type = &vfs_vfat_type;
    mnt.dev = &dev;
    mnt.flags = VFS_MS_RDONLY;
    return vfs_vfat_type.mount(&mnt, &dev, VFS_MS_RDONLY, options);
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
        int rc2 = mount_image("nosuch=1");

        if (rc1 != 0 || rc2 != -EINVAL) {
            printf("fat32_hosttest: umask=0 mounted %d, nosuch=1 mounted %d\n", rc1, rc2);
        }
        check(rc1 == 0 && rc2 == -EINVAL, "an unknown option is EINVAL");
    }
    check(vfs_vfat_type.mount(&mnt, &dev, 0, "") == -EROFS, "a writable mount is EROFS");
}

static void pool_rows(void)
{
    void *held[FAT32_NODES + 1];
    int i, err, took = 0;

    load("--vfs");
    err = mount_image("");
    if (err != 0) {
        printf("fat32_hosttest: the pool row's mount said %d\n", err);
    }
    check(err == 0, "the volume mounts for the pool row");
    /* The root holds one entry and /MANY a second; its files fill the
       thirty that remain, the next lookup is ENOMEM and not a corruption,
       and a put frees a slot the next lookup takes. */
    {
        void *many = walk("/many", &err);
        char name[8];

        check(many != 0, "the directory of forty files is found");
        for (i = 0; many && i < 31; i++) {
            name[0] = 'M';
            name[1] = (char)('0' + i / 10);
            name[2] = (char)('0' + i % 10);
            name[3] = '\0';
            err = OPS->lookup(&mnt, many, name, &held[took]);
            if (err == 0) {
                took++;
            }
        }
        check(took == 30 && err == -ENOMEM,
              "thirty files fill the pool and the next lookup is ENOMEM");
        {
            void *again;

            check(OPS->lookup(&mnt, many, "M05", &again) == 0 && again == held[5],
                  "a held name looked up again is the held node");
            if (again) {
                OPS->put(&mnt, again);
            }
        }
        OPS->put(&mnt, held[--took]);
        check(OPS->lookup(&mnt, many, "M39", &held[took]) == 0,
              "a put frees a slot the next lookup takes");
        took++;
        for (i = 0; i < took; i++) {
            OPS->put(&mnt, held[i]);
        }
        if (many) {
            OPS->put(&mnt, many);
        }
    }
    unmount_image();
}

int wq_wait_event_nointr(wait_queue_t *wq, wq_cond_fn cond, void *arg)
{
    return wq_wait_event(wq, cond, arg);
}

int main(void)
{
    proc_a.pid = 1;
    well_formed_rows();
    small_and_active_rows();
    malformed_rows();
    pool_rows();
    if (fails == 0) {
        printf("fat32_hosttest: OK\n");
    }
    return fails;
}
