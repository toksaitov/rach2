/*
 * ext2_hosttest.c: the ext2 driver over the buffer cache over images the
 * host's mke2fs made.
 *
 * host-deps: rach2/sys/fs/ext2.c rach2/sys/fs/bcache.c
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 *
 * The volumes are built here from a staged tree by the host's mke2fs on the
 * profile mkext2.sh writes, and debugfs adds device nodes in both encodings
 * and a size past the kernel's offset, which a host tree does not have.
 * Every file reads back against the tree, and every node against
 * setidcheck's reader, which shares nothing with this driver. A 1 KiB
 * volume has a sparse file with a byte behind each level of indirection
 * and mounts read-only by name, as a dir_index volume does; an unknown
 * incompatible feature refuses the mount. No hosted headers: the kernel
 * headers define the fixed-width types.
 */
#include "../../rach2/sys/fs/ext2.h"
#include "../../rach2/sys/fs/bcache.h"
#include "../../rach2/sys/lib/kerrno.h"
#include "../../rach2/sys/proc/process.h"

extern int printf(const char *fmt, ...);
extern int snprintf(char *s, unsigned long n, const char *fmt, ...);
extern int system(const char *cmd);
extern void exit(int status);
extern void *fopen(const char *path, const char *mode);
extern int fseek(void *f, long off, int whence);
extern unsigned long fread(void *buf, unsigned long size, unsigned long n, void *f);
extern unsigned long fwrite(const void *buf, unsigned long size, unsigned long n, void *f);
extern int fclose(void *f);
extern char *fgets(char *s, int n, void *f);
extern long strtol(const char *s, char **end, int base);
extern unsigned long strtoul(const char *s, char **end, int base);
extern int strcmp(const char *a, const char *b);
extern int memcmp(const void *a, const void *b, unsigned long n);
extern void *memset(void *s, int c, unsigned long n);

#define FIXDIR    "/tmp/rach2_verify/ext2"
#define SEEK_SET_ 0

/* ---- stubs for what the driver and the cache reach outside themselves ---- */

process_t proc_a;
process_t *current_proc = &proc_a;

void k_panic(const char *s)
{
    printf("ext2_hosttest: panic: %s\n", s);
    exit(3);
}

/* The last record, and the last one at warning level, which is where the
   driver puts a refusal or a read-only reason. */
static char last_log[160];
static char last_warn[160];
extern int vsnprintf(char *s, unsigned long n, const char *fmt, __builtin_va_list ap);
void klog(int level, const char *sys, const char *fmt, ...)
{
    __builtin_va_list ap;

    (void)sys;
    __builtin_va_start(ap, fmt);
    vsnprintf(last_log, sizeof(last_log), fmt, ap);
    __builtin_va_end(ap);
    if (level == 4) {
        __builtin_memcpy(last_warn, last_log, sizeof(last_warn));
    }
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
void vfs_device_failed(block_device_t *d, uint64_t lba, int rc)
{
    (void)d;
    (void)lba;
    (void)rc;
}
int wq_wait_event_nointr(wait_queue_t *wq, wq_cond_fn cond, void *arg)
{
    return wq_wait_event(wq, cond, arg);
}
void vfs_ns_lock(struct vfs_mount *mnt)
{
    (void)mnt;
}
void vfs_ns_unlock(struct vfs_mount *mnt)
{
    (void)mnt;
}

/* ---- the image file as a block device ---- */

static void *image;
static uint64_t image_sectors;

static int img_read(block_device_t *d, uint64_t lba, uint32_t count, void *buf)
{
    (void)d;
    if (lba + count > image_sectors) {
        return -5;
    }
    if (fseek(image, (long)(lba * 512), SEEK_SET_) != 0 ||
        fread(buf, 1, count * 512, image) != count * 512) {
        return -5;
    }
    return 0;
}

static int img_write(block_device_t *d, uint64_t lba, uint32_t count, const void *buf)
{
    (void)d;
    (void)lba;
    (void)count;
    (void)buf;
    return -5;
}

static block_device_t dev = {512, 0, BLOCK_FAULT_SAFE, 0, img_read, img_write, 0, 0, 0};

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
    return strcmp(a, b) == 0;
}

static int run(const char *cmd)
{
    return system(cmd) == 0;
}

/* Open the image at `path` as the device, whatever was open before closed. */
static void use_image(const char *path, uint64_t bytes)
{
    if (image) {
        bcache_sync(&dev);
        fclose(image);
    }
    image = fopen(path, "rb");
    if (!image) {
        printf("ext2_hosttest: %s did not open\n", path);
        exit(1);
    }
    image_sectors = bytes / 512;
    dev.sector_count = image_sectors;
    if (bcache_invalidate(&dev) != 0) {
        printf("ext2_hosttest: the cache did not empty for a new image\n");
        exit(1);
    }
}

static struct vfs_mount mnt;
#define OPS (vfs_ext2_type.ops)

static int mount_image(void)
{
    int rc;

    __builtin_memset(&mnt, 0, sizeof(mnt));
    mnt.type = &vfs_ext2_type;
    mnt.dev = &dev;
    mnt.flags = 0;
    mnt.ns_holder = -1;
    rc = vfs_ext2_type.mount(&mnt, &dev, 0, "");
    return rc;
}

static void unmount_image(void)
{
    if (mnt.root) {
        OPS->put(&mnt, mnt.root);
        OPS->unmount(&mnt);
        mnt.root = 0;
    }
}

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

/* Read `cap` bytes of a file from `offset`; the byte count, or a negative
   errno. */
static int read_at(void *node, unsigned char *buf, uint32_t cap, uint64_t offset)
{
    void *h;
    int32_t got, total = 0;
    int rc = OPS->open(&mnt, node, 0, &h);

    if (rc != 0) {
        return rc;
    }
    for (;;) {
        got = OPS->read(&mnt, h, buf + total, cap - (uint32_t)total, offset + (uint64_t)total);
        if (got < 0) {
            OPS->release(&mnt, h);
            return got;
        }
        if (got == 0 || (uint32_t)total == cap) {
            break;
        }
        total += got;
    }
    OPS->release(&mnt, h);
    return total;
}

/* The bytes of a host file at `offset`; the count read. */
static long host_read(const char *path, unsigned char *buf, unsigned long cap, long offset)
{
    void *f = fopen(path, "rb");
    unsigned long got;

    if (!f) {
        return -1;
    }
    if (fseek(f, offset, SEEK_SET_) != 0) {
        fclose(f);
        return -1;
    }
    got = fread(buf, 1, cap, f);
    fclose(f);
    return (long)got;
}

/* A file in the image against the same file in the tree, whole. */
static unsigned char abuf[1 << 20];
static unsigned char bbuf[1 << 20];
static void same_as_tree(const char *guest, const char *what)
{
    int err;
    void *n = walk(guest, &err);
    char host[512];
    long hgot;
    int igot;

    if (!n) {
        printf("FAIL: %s: %s is not in the image (%d)\n", what, guest, err);
        fails++;
        return;
    }
    snprintf(host, sizeof(host), FIXDIR "/tree%s", guest);
    hgot = host_read(host, bbuf, sizeof(bbuf), 0);
    igot = read_at(n, abuf, sizeof(abuf), 0);
    OPS->put(&mnt, n);
    if (hgot < 0 || igot < 0 || hgot != igot || memcmp(abuf, bbuf, (unsigned long)igot) != 0) {
        printf("FAIL: %s: %s reads %d bytes, the tree has %ld\n", what, guest, igot, hgot);
        fails++;
    }
}

/* Every node setidcheck's reader lists against the driver's own view. */
static void against_reader(const char *img)
{
    char cmd[1024];
    char line[1024];
    void *f;
    int listed = 0, agreed = 0;

    snprintf(cmd, sizeof(cmd),
             "build/tools/inspect/setidcheck --nodes --ext2 %s > " FIXDIR "/nodes.txt", img);
    if (!run(cmd)) {
        check(0, "setidcheck lists the image's nodes");
        return;
    }
    f = fopen(FIXDIR "/nodes.txt", "r");
    if (!f) {
        check(0, "the node list opens");
        return;
    }
    while (fgets(line, sizeof(line), f)) {
        char path[512];
        unsigned long mode, uid, gid, size, ino;
        char *p = line;
        int i = 0;
        int err;
        void *n;
        struct vfs_stat st;

        while (*p && *p != ' ' && i < 511) {
            path[i++] = *p++;
        }
        path[i] = '\0';
        mode = strtoul(p, &p, 8);
        uid = strtoul(p, &p, 10);
        gid = strtoul(p, &p, 10);
        size = strtoul(p, &p, 10);
        ino = strtoul(p, &p, 10);
        listed++;
        n = walk(path, &err);
        if (!n) {
            printf("FAIL: the reader lists %s and the driver cannot find it (%d)\n", path, err);
            fails++;
            continue;
        }
        if (OPS->getattr(&mnt, n, &st) == 0 && st.mode == mode && st.uid == uid && st.gid == gid &&
            st.size == size && st.ino == ino) {
            agreed++;
        } else {
            printf(
                "FAIL: %s: the driver says %lo %lu %lu %lu %lu, the reader %lo %lu %lu %lu %lu\n",
                path, (unsigned long)st.mode, (unsigned long)st.uid, (unsigned long)st.gid,
                (unsigned long)st.size, (unsigned long)st.ino, mode, uid, gid, size, ino);
            fails++;
        }
        OPS->put(&mnt, n);
    }
    fclose(f);
    check(listed > 0 && agreed == listed, "every node the reader lists agrees with the driver");
}

#define PROFILE                                                                         \
    "sparse_super,large_file,filetype,^dir_index,^resize_inode,^ext_attr,^has_journal," \
    "^extent,^64bit,^flex_bg,^metadata_csum,^huge_file,^dir_nlink,^extra_isize"

/* The staged tree. */
static void build_tree(void)
{
    run("rm -rf " FIXDIR "; mkdir -p " FIXDIR "/tree/dir/sub " FIXDIR "/tree/many");
    run("printf 'hello\\n' > " FIXDIR "/tree/hello.txt");
    run("printf 'deep\\n' > " FIXDIR "/tree/dir/sub/deep.txt");
    run("head -c 200000 /dev/urandom > " FIXDIR "/tree/dir/big.bin");
    run("ln -s hello.txt " FIXDIR "/tree/fast");
    run("ln -s "
        "dir/sub/deep.txt/../../../dir/sub/deep.txt/../../sub/deep.txt/../deep.txt/../"
        "deep.txt " FIXDIR "/tree/slow");
    run("ln " FIXDIR "/tree/hello.txt " FIXDIR "/tree/hello.link");
    run("dd if=/dev/zero of=" FIXDIR "/tree/sparse bs=1 count=0 seek=5000000 2>/dev/null; "
        "printf 'end' >> " FIXDIR "/tree/sparse");
    run("i=0; while [ $i -lt 300 ]; do printf 'x' > " FIXDIR
        "/tree/many/file$i.txt; i=$((i+1)); done");
    /* A sparse file with one byte behind each pointer level at 1 KiB
       blocks, where the direct blocks end at 12 KiB, the indirect at
       268 KiB and the double-indirect at 64 MiB. It has a tree of its own,
       since the guard's reader refuses a 70 MB file. */
    run("mkdir -p " FIXDIR "/levels; "
        "dd if=/dev/zero of=" FIXDIR "/levels/levels bs=1 count=0 seek=70000000 2>/dev/null; "
        "printf 'A' | dd of=" FIXDIR "/levels/levels bs=1 seek=0 conv=notrunc 2>/dev/null; "
        "printf 'B' | dd of=" FIXDIR "/levels/levels bs=1 seek=12388 conv=notrunc 2>/dev/null; "
        "printf 'C' | dd of=" FIXDIR "/levels/levels bs=1 seek=300000 conv=notrunc 2>/dev/null; "
        "printf 'D' | dd of=" FIXDIR "/levels/levels bs=1 seek=69000000 conv=notrunc 2>/dev/null");
}

/* Format `img` from the tree with the block and inode sizes given and
   debugfs's additions; the image size in bytes. */
static uint64_t build_image(const char *img, int block, int inode, const char *features,
                            unsigned long mib, const char *tree)
{
    char cmd[2048];

    snprintf(cmd, sizeof(cmd),
             ". tools/setup/e2fsprogs_path.sh; rm -f %s; : > %s; "
             "E2FSPROGS_FAKE_TIME=1 mke2fs -q -F -t ext2 -b %d -I %d -O %s -d " FIXDIR
             "/%s %s %luM >/dev/null 2>&1 && "
             "debugfs -w -R 'mknod cold c 4 5' %s >/dev/null 2>&1 && "
             "debugfs -w -R 'mknod cnew c 1 1' %s >/dev/null 2>&1 && "
             "debugfs -w -R 'set_inode_field /cnew block[0] 0' %s >/dev/null 2>&1 && "
             "debugfs -w -R 'set_inode_field /cnew block[1] 286338160' %s >/dev/null 2>&1 && "
             "debugfs -w -R 'mknod bdev b 8 1' %s >/dev/null 2>&1 && "
             "debugfs -w -R 'set_inode_field /hello.txt uid 7' %s >/dev/null 2>&1 && "
             "debugfs -w -R 'set_inode_field /hello.txt gid 9' %s >/dev/null 2>&1 && "
             "debugfs -w -R 'set_inode_field /hello.txt mode 0104755' %s >/dev/null 2>&1",
             img, img, block, inode, features, tree, img, mib, img, img, img, img, img, img, img,
             img);
    if (!run(cmd)) {
        printf("ext2_hosttest: mke2fs or debugfs failed for %s\n", img);
        exit(1);
    }
    return (uint64_t)mib << 20;
}

static void read_rows(const char *img, uint64_t bytes, const char *what, int on_profile)
{
    struct vfs_stat st;
    void *n;
    int err;
    char linkbuf[300];

    use_image(img, bytes);
    check(mount_image() == 0, what);
    if (!mnt.root) {
        return;
    }
    same_as_tree("/hello.txt", what);
    same_as_tree("/dir/sub/deep.txt", what);
    same_as_tree("/dir/big.bin", what);
    same_as_tree("/hello.link", what);
    same_as_tree("/sparse", what);

    n = walk("/hello.txt", &err);
    check(n && OPS->getattr(&mnt, n, &st) == 0 && st.mode == 0104755 && st.uid == 7 &&
              st.gid == 9 && st.nlink == 2 && st.size == 6,
          "hello.txt has the mode, owner, link count and size debugfs wrote");
    if (n) {
        OPS->put(&mnt, n);
    }
    n = walk("/dir", &err);
    check(n && OPS->getattr(&mnt, n, &st) == 0 && (st.mode & 0170000) == 0040000 && st.nlink == 3,
          "a directory reports its type and its two subdirectories' links");
    if (n) {
        char path[256];

        check(OPS->path(&mnt, n, path, sizeof(path)) == 0 && streq(path, "/dir"),
              "a directory's path is rebuilt from .. and the parent's entry");
        OPS->put(&mnt, n);
    }
    n = walk("/dir/sub", &err);
    if (n) {
        char path[256];

        check(OPS->path(&mnt, n, path, sizeof(path)) == 0 && streq(path, "/dir/sub"),
              "two levels down as well");
        OPS->put(&mnt, n);
    }

    n = walk("/fast", &err);
    check(n && OPS->getattr(&mnt, n, &st) == 0 && (st.mode & 0170000) == 0120000 &&
              OPS->readlink(&mnt, n, linkbuf, sizeof(linkbuf)) == 9 && streq(linkbuf, "hello.txt"),
          "a fast link is read from the inode");
    if (n) {
        OPS->put(&mnt, n);
    }
    n = walk("/slow", &err);
    check(n && OPS->readlink(&mnt, n, linkbuf, sizeof(linkbuf)) > 60 &&
              memcmp(linkbuf, "dir/sub/deep.txt/../../../dir/sub", 33) == 0,
          "a slow link is read from its block");
    check(n && OPS->readlink(&mnt, n, linkbuf, 10) == -ENAMETOOLONG,
          "a link longer than the buffer answers ENAMETOOLONG");
    if (n) {
        OPS->put(&mnt, n);
    }

    /* debugfs writes the old device encoding only, so the new one, major
       300 and minor 70000 as 0x11112c70 in the second word, is written by
       field. */
    n = walk("/cold", &err);
    check(n && OPS->getattr(&mnt, n, &st) == 0 && (st.mode & 0170000) == 0020000 &&
              st.rdev == ((4u << 8) | 5u),
          "a device in the old encoding reports its numbers");
    if (n) {
        void *h;

        check(OPS->open(&mnt, n, 0, &h) == -EACCES, "and cannot be opened");
        OPS->put(&mnt, n);
    }
    n = walk("/cnew", &err);
    if (n && OPS->getattr(&mnt, n, &st) == 0 &&
        st.rdev != ((300u << 8) | (70000u & 0xFFu) | ((70000u & 0xFFF00u) << 12))) {
        printf("  cnew: rdev %lx\n", (unsigned long)st.rdev);
    }
    check(n && OPS->getattr(&mnt, n, &st) == 0 &&
              st.rdev == ((300u << 8) | (70000u & 0xFFu) | ((70000u & 0xFFF00u) << 12)),
          "a device in the new encoding reports its numbers");
    if (n) {
        OPS->put(&mnt, n);
    }
    n = walk("/bdev", &err);
    check(n && OPS->getattr(&mnt, n, &st) == 0 && (st.mode & 0170000) == 0060000,
          "a block device is a block device");
    if (n) {
        OPS->put(&mnt, n);
    }

    n = walk("/many", &err);
    if (n) {
        void *h;
        uint32_t cursor = 0;
        int count = 0, got;
        struct vfs_dirent de;

        if (OPS->open(&mnt, n, 0, &h) == 0) {
            while ((got = OPS->readdir(&mnt, h, &cursor, &de)) == 1) {
                if (!streq(de.name, ".") && !streq(de.name, "..")) {
                    count++;
                }
            }
            OPS->release(&mnt, h);
            check(got == 0 && count == 300, "a directory across blocks lists its 300 entries");
        }
        OPS->put(&mnt, n);
    }
    n = walk("/many/file299.txt", &err);
    check(n != 0, "the last entry of the spanning directory is found");
    if (n) {
        OPS->put(&mnt, n);
    }
    check(walk("/many/file300.txt", &err) == 0 && err == -ENOENT, "a name that is not there");
    check(walk("/hello.txt/x", &err) == 0 && err == -ENOTDIR, "a file is not a directory");

    n = walk("/sparse", &err);
    if (n) {
        int got = read_at(n, abuf, 16, 4999990);

        check(got == 13 && abuf[0] == 0 && abuf[9] == 0 && memcmp(abuf + 10, "end", 3) == 0,
              "a hole reads as zeros up to the bytes after it");
        OPS->put(&mnt, n);
    }

    if (on_profile) {
        against_reader(img);
    }
    {
        struct vfs_statfs sf;

        check(OPS->statfs(&mnt, &sf) == 0 && sf.magic == 0xEF53 && sf.namelen == 255 &&
                  sf.blocks == bytes / sf.bsize && sf.bfree < sf.blocks,
              "statfs reports the format's magic, the volume's size and a free count");
    }
    unmount_image();
}

/* The 1 KiB volume: every pointer level, read-only by name. */
static void levels_rows(const char *img, uint64_t bytes)
{
    void *n;
    int err;

    use_image(img, bytes);
    check(mount_image() == 0, "the 1 KiB volume mounts");
    check((mnt.flags & VFS_MS_RDONLY) != 0 &&
              memcmp(last_warn, "mounted read-only: a block size below 4096", 42) == 0,
          "and read-only, since a block below 4096 cannot be written in order");
    if (!mnt.root) {
        return;
    }
    n = walk("/levels", &err);
    if (n) {
        unsigned char b[1];

        check(read_at(n, b, 1, 0) == 1 && b[0] == 'A', "a byte through a direct block");
        check(read_at(n, b, 1, 12388) == 1 && b[0] == 'B', "a byte through the indirect block");
        check(read_at(n, b, 1, 300000) == 1 && b[0] == 'C',
              "a byte through the double-indirect block");
        check(read_at(n, b, 1, 69000000) == 1 && b[0] == 'D',
              "a byte through the triple-indirect block");
        check(read_at(n, b, 1, 40000000) == 1 && b[0] == 0, "a hole between them reads as zero");
        OPS->put(&mnt, n);
    } else {
        check(0, "the levels file is in the 1 KiB volume");
    }
    unmount_image();
}

/* Write `len` bytes at `at` of the image file. */
static void poke(const char *img, long at, const unsigned char *bytes, unsigned long len)
{
    void *f = fopen(img, "r+b");

    if (!f || fseek(f, at, SEEK_SET_) != 0 || fwrite(bytes, 1, len, f) != len) {
        printf("ext2_hosttest: cannot poke %s\n", img);
        exit(1);
    }
    fclose(f);
}

static void refusal_rows(void)
{
    static const char img[] = FIXDIR "/mut.ext2";
    unsigned char b[4];
    uint64_t bytes;
    void *n;
    int err;
    char linkbuf[300];

    bytes = build_image(img, 4096, 128,
                        "sparse_super,large_file,filetype,dir_index,^resize_inode,"
                        "^ext_attr,^has_journal,^extent,^64bit,^flex_bg,"
                        "^metadata_csum,^huge_file,^dir_nlink,^extra_isize",
                        16, "tree");
    use_image(img, bytes);
    check(mount_image() == 0 && (mnt.flags & VFS_MS_RDONLY) != 0 &&
              memcmp(last_warn, "mounted read-only: dir_index", 28) == 0,
          "a dir_index volume mounts read-only by name");
    unmount_image();

    bytes = build_image(img, 4096, 128, PROFILE, 16, "tree");
    b[0] = 0x42;
    b[1] = 0;
    b[2] = 0;
    b[3] = 0;
    poke(img, 1024 + 96, b, 4);
    use_image(img, bytes);
    check(mount_image() == -EINVAL &&
              memcmp(last_warn, "refused: incompatible feature 0x40", 34) == 0,
          "an unknown incompatible feature refuses the mount by name");

    bytes = build_image(img, 4096, 128, PROFILE, 16, "tree");
    b[0] = 0x0b; /* sparse_super, large_file and huge_file */
    poke(img, 1024 + 100, b, 4);
    use_image(img, bytes);
    check(mount_image() == 0 && (mnt.flags & VFS_MS_RDONLY) != 0 &&
              memcmp(last_warn, "mounted read-only: unknown read-only", 36) == 0,
          "an unknown read-only-compatible feature mounts read-only by name");
    unmount_image();

    bytes = build_image(img, 4096, 128, PROFILE, 16, "tree");
    run(". tools/setup/e2fsprogs_path.sh; debugfs -w -R 'set_inode_field /dir/big.bin size "
        "5000000000' " FIXDIR "/mut.ext2 >/dev/null 2>&1");
    use_image(img, bytes);
    check(mount_image() == 0, "the volume with the big file mounts");
    n = walk("/dir/big.bin", &err);
    if (n) {
        struct vfs_stat st;
        void *h;

        check(OPS->getattr(&mnt, n, &st) == -EOVERFLOW,
              "a file past the offset answers EOVERFLOW to stat");
        check(OPS->open(&mnt, n, 0, &h) == -EOVERFLOW, "and to open");
        OPS->put(&mnt, n);
    }
    same_as_tree("/hello.txt", "the rest of the volume reads");
    unmount_image();

    bytes = build_image(img, 4096, 128, PROFILE, 16, "tree");
    run(". tools/setup/e2fsprogs_path.sh; debugfs -w -R 'set_inode_field /hello.txt block[0] "
        "9999999' " FIXDIR "/mut.ext2 >/dev/null 2>&1");
    use_image(img, bytes);
    check(mount_image() == 0, "the volume with the bad pointer mounts");
    n = walk("/hello.txt", &err);
    if (n) {
        check(read_at(n, abuf, 16, 0) == -EIO && memcmp(last_log, "inode ", 6) == 0,
              "a block past the volume fails the read by name");
        OPS->put(&mnt, n);
    }
    same_as_tree("/dir/sub/deep.txt", "the rest of the volume reads");
    unmount_image();

    bytes = build_image(img, 4096, 128, PROFILE, 16, "tree");
    run(". tools/setup/e2fsprogs_path.sh; debugfs -w -R 'set_inode_field /hello.txt block[0] "
        "1' " FIXDIR "/mut.ext2 >/dev/null 2>&1");
    use_image(img, bytes);
    check(mount_image() == 0, "the volume with the pointer into a table mounts");
    n = walk("/hello.txt", &err);
    if (n) {
        check(read_at(n, abuf, 16, 0) == -EIO && memcmp(last_warn, "inode ", 6) == 0,
              "a block inside the descriptors fails the read by name");
        OPS->put(&mnt, n);
    }
    unmount_image();

    bytes = build_image(img, 4096, 128, PROFILE, 16, "tree");
    {
        char line[256];
        void *f;
        unsigned long table = 0;

        run(". tools/setup/e2fsprogs_path.sh; dumpe2fs " FIXDIR
            "/mut.ext2 2>/dev/null | grep 'Inode table at' | head -1 > " FIXDIR "/table.txt");
        f = fopen(FIXDIR "/table.txt", "r");
        if (f && fgets(line, sizeof(line), f)) {
            char *p = line;

            while (*p && (*p < '0' || *p > '9')) {
                p++;
            }
            table = strtoul(p, 0, 10);
        }
        if (f) {
            fclose(f);
        }
        if (table != 0) {
            char cmd[512];

            snprintf(cmd, sizeof(cmd),
                     ". tools/setup/e2fsprogs_path.sh; debugfs -w -R 'set_inode_field /hello.txt "
                     "block[0] %lu' " FIXDIR "/mut.ext2 >/dev/null 2>&1",
                     table);
            run(cmd);
            use_image(img, bytes);
            check(mount_image() == 0, "the volume with the pointer into the inode table mounts");
            n = walk("/hello.txt", &err);
            if (n) {
                check(read_at(n, abuf, 16, 0) == -EIO && memcmp(last_warn, "inode ", 6) == 0,
                      "a block inside the inode table fails the read by name");
                OPS->put(&mnt, n);
            }
            unmount_image();
        } else {
            check(0, "dumpe2fs names the inode table for the row");
        }
    }

    bytes = build_image(img, 4096, 128, PROFILE, 16, "tree");
    use_image(img, bytes);
    check(mount_image() == 0, "the volume mounts before its root is damaged");
    if (mnt.root) {
        struct vfs_stat st;
        uint32_t root_block = 0;

        OPS->getattr(&mnt, mnt.root, &st);
        unmount_image();
        /* The root directory's first block, through the reader. */
        {
            char line[256];
            void *f;

            run(". tools/setup/e2fsprogs_path.sh; debugfs -R 'stat /' " FIXDIR
                "/mut.ext2 2>/dev/null | grep -A1 '^BLOCKS' | tail -1 > " FIXDIR "/blk.txt");
            f = fopen(FIXDIR "/blk.txt", "r");
            if (f && fgets(line, sizeof(line), f)) {
                char *p = line;

                while (*p && *p != ':') {
                    p++;
                }
                if (*p == ':') {
                    root_block = (uint32_t)strtoul(p + 1, 0, 10);
                }
            }
            if (f) {
                fclose(f);
            }
        }
        if (root_block != 0) {
            b[0] = 0x00;
            b[1] = 0x20; /* rec_len 8192, past a 4096-byte block */
            poke(img, (long)root_block * 4096 + 4, b, 2);
            use_image(img, bytes);
            check(mount_image() == 0, "the volume with the malformed entry mounts");
            check(walk("/hello.txt", &err) == 0 && err == -EIO &&
                      memcmp(last_warn, "directory 2: entry at 0 is malformed", 36) == 0,
                  "a malformed entry fails the lookup by name");
            unmount_image();
            /* The third entry, the first after . and .., naming an inode
               past the count. */
            bytes = build_image(img, 4096, 128, PROFILE, 16, "tree");
            b[0] = 0xff;
            b[1] = 0xff;
            b[2] = 0xff;
            b[3] = 0x0f;
            poke(img, (long)root_block * 4096 + 24, b, 4);
            use_image(img, bytes);
            check(mount_image() == 0, "the volume with the entry past the count mounts");
            check(walk("/zzz", &err) == 0 && err == -EIO &&
                      memcmp(last_warn, "directory 2: entry at 24 is malformed", 37) == 0,
                  "an entry naming an inode past the count is malformed by name");
            unmount_image();
        } else {
            check(0, "the root's block was found for the malformed-entry row");
        }
    }

    bytes = build_image(img, 4096, 128, PROFILE, 16, "tree");
    b[0] = 0;
    b[1] = 0;
    poke(img, 1024 + 58, b, 2);
    use_image(img, bytes);
    check(mount_image() == 0 && (mnt.flags & VFS_MS_RDONLY) != 0 &&
              memcmp(last_warn, "mounted read-only: not unmounted cleanly", 40) == 0,
          "a volume whose valid bit is clear mounts read-only by name");
    unmount_image();

    /* A revision 0 volume, without filetype: readdir reads each inode. */
    bytes = build_image(img, 4096, 128,
                        "^sparse_super,^large_file,^filetype,^dir_index,^resize_inode,"
                        "^ext_attr,^has_journal,^extent,^64bit,^flex_bg,"
                        "^metadata_csum,^huge_file,^dir_nlink,^extra_isize",
                        16, "tree");
    run(". tools/setup/e2fsprogs_path.sh; debugfs -w -R 'ssv rev_level 0' " FIXDIR
        "/mut.ext2 >/dev/null 2>&1");
    use_image(img, bytes);
    check(mount_image() == 0, "a revision 0 volume mounts");
    same_as_tree("/hello.txt", "a revision 0 volume reads");
    n = walk("/dir", &err);
    if (n) {
        void *h;
        uint32_t cursor = 0;
        int got, dirs = 0, files = 0;
        struct vfs_dirent de;

        if (OPS->open(&mnt, n, 0, &h) == 0) {
            while ((got = OPS->readdir(&mnt, h, &cursor, &de)) == 1) {
                if (streq(de.name, "sub")) {
                    dirs += de.is_dir;
                } else if (streq(de.name, "big.bin")) {
                    files += !de.is_dir;
                }
            }
            OPS->release(&mnt, h);
            check(got == 0 && dirs == 1 && files == 1,
                  "without filetype, readdir tells a directory from a file by its inode");
        }
        OPS->put(&mnt, n);
    }
    unmount_image();

    /* A directory whose size ends inside its first entry. */
    bytes = build_image(img, 4096, 128, PROFILE, 16, "tree");
    run(". tools/setup/e2fsprogs_path.sh; debugfs -w -R 'set_inode_field /dir size 1' " FIXDIR
        "/mut.ext2 >/dev/null 2>&1");
    use_image(img, bytes);
    check(mount_image() == 0, "the volume with the short directory mounts");
    check(walk("/dir/sub", &err) == 0 && err == -EIO && memcmp(last_warn, "directory ", 10) == 0,
          "an entry past the directory's size is malformed by name");
    unmount_image();

    /* A slow link of 1021 bytes, and one whose block is missing. */
    bytes = build_image(img, 4096, 128, PROFILE, 16, "tree");
    run("t=$(printf 'a%.0s' $(seq 1 1021)); ln -s \"$t\" " FIXDIR "/tree/long1021; "
        ". tools/setup/e2fsprogs_path.sh; rm -f " FIXDIR "/mut.ext2; : > " FIXDIR "/mut.ext2; "
        "E2FSPROGS_FAKE_TIME=1 mke2fs -q -F -t ext2 -b 4096 -I 128 -O " PROFILE " -d " FIXDIR
        "/tree " FIXDIR "/mut.ext2 16M >/dev/null 2>&1; rm -f " FIXDIR "/tree/long1021");
    use_image(img, bytes);
    check(mount_image() == 0, "the volume with the long link mounts");
    n = walk("/long1021", &err);
    if (n) {
        static char big[2048];

        check(OPS->readlink(&mnt, n, big, sizeof(big)) == 1021 && big[0] == 'a' && big[1020] == 'a',
              "a slow link of 1021 bytes is read from its block");
        OPS->put(&mnt, n);
    } else {
        check(0, "the long link is in the volume");
    }
    unmount_image();
    bytes = build_image(img, 4096, 128, PROFILE, 16, "tree");
    run(". tools/setup/e2fsprogs_path.sh; debugfs -w -R 'set_inode_field /slow block[0] 0' " FIXDIR
        "/mut.ext2 >/dev/null 2>&1");
    use_image(img, bytes);
    check(mount_image() == 0, "the volume with the blockless slow link mounts");
    n = walk("/slow", &err);
    if (n) {
        check(OPS->readlink(&mnt, n, linkbuf, sizeof(linkbuf)) == -EIO,
              "a slow link without a block answers EIO");
        OPS->put(&mnt, n);
    }
    unmount_image();

    /* A group whose free count is past its size, and tables placed where
       a group's tables cannot be. */
    bytes = build_image(img, 4096, 128, PROFILE, 16, "tree");
    run(". tools/setup/e2fsprogs_path.sh; debugfs -w -R 'set_bg 0 free_blocks_count 65535' " FIXDIR
        "/mut.ext2 >/dev/null 2>&1");
    use_image(img, bytes);
    check(mount_image() == -EINVAL && streq(last_warn, "refused: group free count"),
          "a free count past the group is refused by name");
    bytes = build_image(img, 4096, 128, PROFILE, 16, "tree");
    run(". tools/setup/e2fsprogs_path.sh; debugfs -w -R 'set_bg 0 block_bitmap 1' " FIXDIR
        "/mut.ext2 >/dev/null 2>&1");
    use_image(img, bytes);
    check(mount_image() == -EINVAL && streq(last_warn, "refused: group descriptor"),
          "a bitmap on the descriptors' block is refused by name");
    bytes = build_image(img, 4096, 128, PROFILE, 16, "tree");
    run(". tools/setup/e2fsprogs_path.sh; debugfs -w -R 'set_bg 0 inode_table 0' " FIXDIR
        "/mut.ext2 >/dev/null 2>&1");
    use_image(img, bytes);
    check(mount_image() == -EINVAL && streq(last_warn, "refused: group descriptor"),
          "an inode table at block zero is refused by name");

    /* Sixty-four nodes held, and the sixty-fifth refused. */
    bytes = build_image(img, 4096, 128, PROFILE, 16, "tree");
    use_image(img, bytes);
    check(mount_image() == 0, "the volume mounts for the pool row");
    if (mnt.root) {
        void *held[EXT2_NODES];
        int took = 0, i;
        void *many = walk("/many", &err);

        for (i = 0; many && i < EXT2_NODES; i++) {
            char name[32];
            void *node;

            snprintf(name, sizeof(name), "file%d.txt", i);
            if (OPS->lookup(&mnt, many, name, &node) != 0) {
                break;
            }
            held[took++] = node;
        }
        /* The root and /many are held too, so the pool fills two short of
           the count. */
        check(took == EXT2_NODES - 2,
              "the pool holds sixty-two files beside the root and their directory");
        if (many) {
            void *node;

            check(OPS->lookup(&mnt, many, "file63.txt", &node) == -ENOMEM,
                  "the sixty-fifth node answers ENOMEM");
            for (i = 0; i < took; i++) {
                OPS->put(&mnt, held[i]);
            }
            check(OPS->lookup(&mnt, many, "file63.txt", &node) == 0,
                  "and is found once one is put");
            OPS->put(&mnt, node);
            OPS->put(&mnt, many);
        }
    }
    unmount_image();

    /* A size of 0xffffffff, the last the kernel's offset holds. */
    bytes = build_image(img, 4096, 128, PROFILE, 16, "tree");
    run(". tools/setup/e2fsprogs_path.sh; debugfs -w -R 'set_inode_field /sparse size "
        "4294967295' " FIXDIR "/mut.ext2 >/dev/null 2>&1");
    use_image(img, bytes);
    check(mount_image() == 0, "the volume with the largest file mounts");
    n = walk("/sparse", &err);
    if (n) {
        struct vfs_stat st;

        check(OPS->getattr(&mnt, n, &st) == 0 && st.size == 4294967295ull,
              "a size of 4 GiB less one is reported");
        check(read_at(n, abuf, 16, 4294967290ull) == 5 && abuf[0] == 0 && abuf[4] == 0,
              "and its last five bytes read as the hole they are");
        OPS->put(&mnt, n);
    }
    unmount_image();

    bytes = build_image(img, 4096, 128, PROFILE, 16, "tree");
    b[0] = 0;
    b[1] = 0;
    poke(img, 1024 + 56, b, 2);
    use_image(img, bytes);
    check(mount_image() == -EINVAL && streq(last_warn, "refused: magic"), "no magic is refused");
}

int main(void)
{
    uint64_t bytes;

    if (!run("mkdir -p " FIXDIR) ||
        !run(". tools/setup/e2fsprogs_path.sh; command -v mke2fs >/dev/null")) {
        printf("ext2_hosttest: mke2fs is not on the path; e2fsprogs is a required host tool\n");
        return 1;
    }
    if (!run("test -x build/tools/inspect/setidcheck || "
             "cc -O2 -o build/tools/inspect/setidcheck tools/inspect/setidcheck.c")) {
        printf("ext2_hosttest: setidcheck did not build\n");
        return 1;
    }
    build_tree();

    bytes = build_image(FIXDIR "/a.ext2", 4096, 128, PROFILE, 16, "tree");
    read_rows(FIXDIR "/a.ext2", bytes, "the 4 KiB volume with 128-byte inodes mounts", 1);
    bytes = build_image(FIXDIR "/b.ext2", 4096, 256, PROFILE, 16, "tree");
    read_rows(FIXDIR "/b.ext2", bytes, "the 4 KiB volume with 256-byte inodes mounts", 0);
    bytes = build_image(FIXDIR "/c.ext2", 2048, 128, PROFILE, 16, "tree");
    read_rows(FIXDIR "/c.ext2", bytes, "the 2 KiB volume mounts", 0);
    bytes = build_image(FIXDIR "/d.ext2", 1024, 128, PROFILE, 96, "levels");
    levels_rows(FIXDIR "/d.ext2", bytes);
    refusal_rows();

    if (image) {
        fclose(image);
    }
    if (fails) {
        printf("ext2_hosttest: %d failure(s)\n", fails);
        return 1;
    }
    printf("ext2_hosttest: PASS\n");
    return 0;
}
