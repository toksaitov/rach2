/*
 * ext2_hosttest.c: the ext2 driver over the buffer cache over images the
 * host's mke2fs made.
 *
 * host-deps: rach2/sys/fs/ext2.c rach2/sys/fs/bcache.c rach2/sys/fs/fatcommon.c
 * host-deps: rach2/sys/block/chain.c
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
extern char *strstr(const char *s, const char *needle);
extern unsigned long strlen(const char *s);
extern long atol(const char *s);
extern long strtol(const char *s, char **end, int base);
extern unsigned long strtoul(const char *s, char **end, int base);
extern int strcmp(const char *a, const char *b);
extern int open(const char *path, int flags, ...);
extern int close(int fd);
extern int unlink(const char *path);
extern unsigned sleep(unsigned seconds);
extern int atexit(void (*fn)(void));
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
static char last_check[160]; /* the check's own record, before the read-only reason */
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
    if (memcmp(last_log, "check: ", 7) == 0) {
        __builtin_memcpy(last_check, last_log, sizeof(last_check));
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
    mnt->ns_holder = (int)current_proc->pid;
    mnt->ns_depth++;
}
void vfs_ns_unlock(struct vfs_mount *mnt)
{
    if (mnt->ns_depth > 0) {
        mnt->ns_depth--;
    }
    if (mnt->ns_depth == 0) {
        mnt->ns_holder = -1;
    }
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

/* Writes land in the image file. `stop_after` is the interruption matrix's
   crash: the writes after that many fail without touching the image, which
   leaves it exactly as a machine stopped after the Nth write would. */
static int writes_made;
static int stop_after = -1;

static int img_write(block_device_t *d, uint64_t lba, uint32_t count, const void *buf)
{
    (void)d;
    if (lba + count > image_sectors) {
        return -5;
    }
    if (stop_after >= 0 && writes_made >= stop_after) {
        return -5;
    }
    writes_made++;
    if (fseek(image, (long)(lba * 512), SEEK_SET_) != 0 ||
        fwrite(buf, 1, count * 512, image) != count * 512) {
        return -5;
    }
    return 0;
}

void rtc_realtime(int64_t *seconds, int64_t *nanoseconds)
{
    *seconds = 1000000000;
    *nanoseconds = 0;
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
    image = fopen(path, "r+b");
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

static int mount_flags(uint32_t flags)
{
    int rc;

    __builtin_memset(&mnt, 0, sizeof(mnt));
    mnt.type = &vfs_ext2_type;
    mnt.dev = &dev;
    mnt.flags = flags;
    mnt.ns_holder = -1;
    rc = vfs_ext2_type.mount(&mnt, &dev, flags, "");
    return rc;
}

static int mount_image(void)
{
    return mount_flags(0);
}

/* A read-only mount: no check, nothing written, the crash state as it is. */
static int mount_ro(void)
{
    return mount_flags(VFS_MS_RDONLY);
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
    last_check[0] = 0;
    check(mount_ro() == 0 && (mnt.flags & VFS_MS_RDONLY) != 0 && last_check[0] == 0,
          "a volume whose valid bit is clear is not checked for a read-only mount");
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

/* ---- the writer's rows ---- */

static int mount_rw(void)
{
    int rc;

    __builtin_memset(&mnt, 0, sizeof(mnt));
    mnt.type = &vfs_ext2_type;
    mnt.dev = &dev;
    mnt.flags = 0;
    mnt.ns_holder = -1;
    rc = vfs_ext2_type.mount(&mnt, &dev, 0, "");
    if (rc == 0) {
        vfs_ns_lock(&mnt); /* the test stands in for the syscall layer */
    }
    return rc;
}

/* The mount's own unmount, through prepare and the cache's sync. */
static int unmount_rw(void)
{
    int rc = 0;

    if (mnt.root) {
        rc = OPS->prepare_unmount(&mnt);
        if (rc == 0) {
            rc = bcache_sync(&dev);
        }
        OPS->put(&mnt, mnt.root);
        OPS->unmount(&mnt);
        mnt.root = 0;
    }
    return rc;
}

static void *dir_of(const char *path, int *err)
{
    return walk(path, err);
}

/* Create `name` under `dir` and return it held. */
static void *make_file(void *dir, const char *name, int *rc)
{
    void *n = 0;

    *rc = OPS->create(&mnt, dir, name, 0644, &n);
    return *rc == 0 ? n : 0;
}

static int write_at(void *node, const void *buf, uint32_t len, uint64_t off)
{
    void *h;
    int32_t got;
    int rc = OPS->open(&mnt, node, 1, &h);

    if (rc != 0) {
        return rc;
    }
    got = OPS->write(&mnt, h, buf, len, off);
    OPS->release(&mnt, h);
    return got < 0 ? got : (got == (int32_t)len ? 0 : -1);
}

/* The bytes of a file in the image against `want`. */
static int file_is(const char *path, const unsigned char *want, uint32_t len)
{
    int err, got;
    void *n = walk(path, &err);

    if (!n) {
        return 0;
    }
    got = read_at(n, abuf, sizeof(abuf), 0);
    OPS->put(&mnt, n);
    if (got != (int)len) {
        printf("  %s: %d bytes, %u wanted\n", path, got, (unsigned)len);
        return 0;
    }
    if (len != 0 && memcmp(abuf, want, len) != 0) {
        uint32_t i = 0;

        while (i < len && abuf[i] == want[i]) {
            i++;
        }
        printf("  %s: byte %u is %d, %d wanted\n", path, (unsigned)i, abuf[i], want[i]);
        return 0;
    }
    return 1;
}

static unsigned char pattern[300000];

static void fill_pattern(void)
{
    unsigned i;

    for (i = 0; i < sizeof(pattern); i++) {
        pattern[i] = (unsigned char)((i * 31 + 7) & 0xFF);
    }
}

/* The three judges of a finished sequence: e2fsck -fn clean, the guard's
   reader walking every node, and debugfs listing the root. */
static int e2fsck_clean(const char *img)
{
    char cmd[512];

    snprintf(cmd, sizeof(cmd),
             ". tools/setup/e2fsprogs_path.sh; e2fsck -fn %s > " FIXDIR "/fsck.out 2>&1", img);
    return system(cmd) == 0;
}

/* e2fsck -fn's exit status: 0 clean, 4 with faults left, 8 when it could
   not run. */
static int e2fsck_status(const char *img)
{
    char cmd[512];
    int rc;

    snprintf(cmd, sizeof(cmd),
             ". tools/setup/e2fsprogs_path.sh; e2fsck -fn %s > " FIXDIR "/fsck.out 2>&1", img);
    rc = system(cmd);
    /* Each question e2fsck asked, with the message before a bare one. */
    system(
        "awk '/[?] no$/ { if ($0 ~ /^(Fix|Clear|Allocate|Salvage|Connect to .lost.found)[?] no$/) "
        "print prev; else print $0 } /./ { prev = $0 }' " FIXDIR "/fsck.out > " FIXDIR
        "/findings.txt");
    return (rc >> 8) & 0xFF;
}

/* The repairs the order of writes allows at a stop: counts, bitmaps, a
   size or block count behind its pointers, an orphan, and the topology of
   a directory moved between parents. `#` in a pattern is a run of digits;
   anything else e2fsck asks about is a state the order does not allow. */
static const char *const allowed_findings[] = {
    "Free inodes count wrong",
    "Free blocks count wrong",
    "Directories count wrong",
    "Block bitmap differences",
    "Inode bitmap differences",
    "Inode #, i_size is",
    "Inode #, i_blocks is",
    "Inode # ref count is",
    "Deleted inode # has zero dtime",
    "Unattached inode #",
    "Unattached zero-length inode #",
    "Unconnected directory inode #",
    "'..' in ",
    "Entry '",
};

static int matches_pattern(const char *line, const char *pat)
{
    while (*pat) {
        if (*pat == '#') {
            if (*line < '0' || *line > '9') {
                return 0;
            }
            while (*line >= '0' && *line <= '9') {
                line++;
            }
            pat++;
        } else {
            if (*line != *pat) {
                return 0;
            }
            line++;
            pat++;
        }
    }
    return 1;
}

/* The findings a sequence's stops may leave: the allowed kinds, and a link
   count only where the sequence writes one apart from its entry. */
static int findings_allowed(int counts)
{
    void *f = fopen(FIXDIR "/findings.txt", "r");
    char line[512];
    int ok = 1;

    if (!f) {
        return 0;
    }
    while (fgets(line, sizeof(line), f)) {
        unsigned k;
        int any = 0;

        for (k = 0; k < sizeof(allowed_findings) / sizeof(allowed_findings[0]); k++) {
            if (matches_pattern(line, allowed_findings[k]) &&
                (counts || !matches_pattern(line, "Inode # ref count is"))) {
                any = 1;
            }
        }
        if (!any) {
            printf("  a repair the order of writes does not allow: %s", line);
            ok = 0;
        }
    }
    fclose(f);
    return ok;
}

static int reader_walks(const char *img)
{
    char cmd[512];

    snprintf(cmd, sizeof(cmd),
             "build/tools/inspect/setidcheck --nodes --ext2 %s > " FIXDIR "/nodes.txt 2>&1", img);
    return system(cmd) == 0;
}

static int debugfs_lists(const char *img, const char *dir, const char *name)
{
    char cmd[600];

    snprintf(
        cmd, sizeof(cmd),
        ". tools/setup/e2fsprogs_path.sh; debugfs -R 'ls -l %s' %s 2>/dev/null | grep -q ' %s$'",
        dir, img, name);
    return system(cmd) == 0;
}

/* The sequences, each on a fresh volume; a sequence returns 0 and leaves
   the volume as its oracle describes. A sequence that renames or links a
   file that already has a name records the write count at which that
   operation begins, so that the matrix can require a name at every stop
   after it. */
static int op_mark = -1;
static const char wimg[] = FIXDIR "/w.ext2";
static uint64_t wbytes;

static int seq_create_write(void)
{
    int rc;
    void *n = make_file(mnt.root, "new.txt", &rc);

    if (!n) {
        return rc;
    }
    op_mark = writes_made; /* the name is committed: it stays at every stop after */
    rc = write_at(n, pattern, 100, 0);
    if (rc == 0) {
        rc = write_at(n, pattern, 10000, 100);
    }
    OPS->put(&mnt, n);
    return rc;
}

/* Across the twelve direct blocks into the indirect, and further. */
static int seq_extend(void)
{
    int rc;
    void *n = make_file(mnt.root, "big.bin", &rc);

    if (!n) {
        return rc;
    }
    rc = write_at(n, pattern, 40000, 0);
    if (rc == 0) {
        rc = write_at(n, pattern + 40000, 20000, 40000);
    }
    if (rc == 0) {
        rc = write_at(n, pattern + 60000, 40000, 60000);
    }
    OPS->put(&mnt, n);
    return rc;
}

static int seq_past_end(void)
{
    int rc;
    void *n = make_file(mnt.root, "holey.bin", &rc);

    if (!n) {
        return rc;
    }
    rc = write_at(n, pattern, 16, 100000);
    OPS->put(&mnt, n);
    return rc;
}

static int seq_truncate(void)
{
    int rc;
    void *n = make_file(mnt.root, "cut.bin", &rc);
    void *h;

    if (!n) {
        return rc;
    }
    rc = write_at(n, pattern, 40000, 0);
    if (rc == 0) {
        rc = OPS->open(&mnt, n, 1, &h);
        if (rc == 0) {
            rc = OPS->truncate(&mnt, h, 5000);
            if (rc == 0) {
                rc = OPS->truncate(&mnt, h, 7000);
            }
            OPS->release(&mnt, h);
        }
    }
    OPS->put(&mnt, n);
    return rc;
}

static int seq_unlink(void)
{
    int rc;
    void *n = make_file(mnt.root, "gone.txt", &rc);

    if (!n) {
        return rc;
    }
    rc = write_at(n, pattern, 12000, 0);
    OPS->put(&mnt, n);
    if (rc == 0) {
        rc = OPS->unlink(&mnt, mnt.root, "gone.txt");
    }
    return rc;
}

static int seq_mkdir_rmdir(void)
{
    int rc = OPS->mkdir(&mnt, mnt.root, "newdir", 0755);
    int err;
    void *d;

    if (rc != 0) {
        return rc;
    }
    op_mark = writes_made;
    d = walk("/newdir", &err);
    if (!d) {
        return err;
    }
    rc = OPS->mkdir(&mnt, d, "inner", 0700);
    OPS->put(&mnt, d);
    if (rc == 0) {
        rc = OPS->mkdir(&mnt, mnt.root, "olddir", 0755);
    }
    if (rc == 0) {
        rc = OPS->rmdir(&mnt, mnt.root, "olddir");
    }
    return rc;
}

static int seq_rename(void)
{
    int rc;
    void *n = make_file(mnt.root, "from.txt", &rc);
    void *d;
    int err;

    if (!n) {
        return rc;
    }
    rc = write_at(n, pattern, 3000, 0);
    OPS->put(&mnt, n);
    op_mark = writes_made;
    if (rc == 0) {
        rc = OPS->rename(&mnt, mnt.root, "from.txt", mnt.root, "to.txt");
    }
    if (rc == 0) {
        d = walk("/dir", &err);
        if (!d) {
            return err;
        }
        rc = OPS->rename(&mnt, mnt.root, "to.txt", d, "moved.txt");
        OPS->put(&mnt, d);
    }
    return rc;
}

static int seq_rename_over(void)
{
    int rc;
    void *n = make_file(mnt.root, "src.txt", &rc);

    if (!n) {
        return rc;
    }
    rc = write_at(n, pattern, 2000, 0);
    OPS->put(&mnt, n);
    if (rc == 0) {
        n = make_file(mnt.root, "victim.txt", &rc);
        if (!n) {
            return rc;
        }
        rc = write_at(n, pattern + 100, 9000, 0);
        OPS->put(&mnt, n);
    }
    op_mark = writes_made;
    if (rc == 0) {
        rc = OPS->rename(&mnt, mnt.root, "src.txt", mnt.root, "victim.txt");
    }
    return rc;
}

static int seq_rename_dir(void)
{
    int rc = OPS->mkdir(&mnt, mnt.root, "mover", 0755);
    void *d, *sub;
    int err;

    if (rc != 0) {
        return rc;
    }
    d = walk("/mover", &err);
    if (!d) {
        return err;
    }
    {
        void *n = make_file(d, "inside.txt", &rc);

        if (n) {
            rc = write_at(n, pattern, 500, 0);
            OPS->put(&mnt, n);
        }
    }
    OPS->put(&mnt, d);
    if (rc != 0) {
        return rc;
    }
    /* Into another parent, and then over an empty directory. */
    sub = walk("/dir/sub", &err);
    if (!sub) {
        return err;
    }
    op_mark = writes_made;
    rc = OPS->rename(&mnt, mnt.root, "mover", sub, "moved");
    if (rc == 0) {
        rc = OPS->mkdir(&mnt, mnt.root, "empty", 0755);
    }
    if (rc == 0) {
        rc = OPS->rename(&mnt, sub, "moved", mnt.root, "empty");
    }
    OPS->put(&mnt, sub);
    return rc;
}

static int seq_link(void)
{
    int rc;
    void *n = make_file(mnt.root, "one.txt", &rc);

    if (!n) {
        return rc;
    }
    rc = write_at(n, pattern, 700, 0);
    op_mark = writes_made;
    if (rc == 0) {
        rc = OPS->link(&mnt, mnt.root, "two.txt", n);
    }
    OPS->put(&mnt, n);
    if (rc == 0) {
        rc = OPS->unlink(&mnt, mnt.root, "one.txt");
    }
    return rc;
}

static int seq_symlink(void)
{
    static char longtarget[200];
    int rc, i;

    for (i = 0; i < 199; i++) {
        longtarget[i] = 't';
    }
    longtarget[199] = '\0';
    rc = OPS->symlink(&mnt, mnt.root, "quick", "hello.txt");
    if (rc == 0) {
        op_mark = writes_made;
        rc = OPS->symlink(&mnt, mnt.root, "lengthy", longtarget);
    }
    return rc;
}

/* A file unlinked while open, written through, and closed. */
static int seq_open_unlinked(void)
{
    int rc;
    void *n = make_file(mnt.root, "temp.txt", &rc);
    void *h;

    if (!n) {
        return rc;
    }
    rc = OPS->open(&mnt, n, 1, &h);
    if (rc == 0) {
        rc = OPS->unlink(&mnt, mnt.root, "temp.txt");
        if (rc == 0) {
            rc = write_at(n, pattern, 8000, 0);
        }
        if (rc == 0) {
            int got = read_at(n, abuf, 8000, 0);

            rc = got == 8000 && memcmp(abuf, pattern, 8000) == 0 ? 0 : -1;
        }
        OPS->release(&mnt, h);
    }
    OPS->put(&mnt, n);
    return rc;
}

/* Three hundred files, which grows the root across blocks, then half
   removed. */
static int seq_many(void)
{
    int i, rc = 0;

    for (i = 0; i < 300 && rc == 0; i++) {
        char name[32];
        void *n;

        snprintf(name, sizeof(name), "m%d.txt", i);
        n = make_file(mnt.root, name, &rc);
        if (n) {
            rc = write_at(n, pattern, 10, 0);
            OPS->put(&mnt, n);
        }
    }
    for (i = 0; i < 300 && rc == 0; i += 2) {
        char name[32];

        snprintf(name, sizeof(name), "m%d.txt", i);
        rc = OPS->unlink(&mnt, mnt.root, name);
    }
    return rc;
}

/* `counts` marks a sequence whose order writes a link count apart from the
   entry it counts: only there may a stop leave a count wrong. */
struct sequence {
    const char *name;
    int (*run)(void);
    int counts;
};

static const struct sequence sequences[] = {
    {"create and write", seq_create_write, 0},
    {"extend through the pointer levels", seq_extend, 0},
    {"write past the end", seq_past_end, 0},
    {"truncate", seq_truncate, 0},
    {"unlink", seq_unlink, 0},
    {"mkdir and rmdir", seq_mkdir_rmdir, 1},
    {"rename", seq_rename, 1},
    {"rename over a file", seq_rename_over, 1},
    {"rename a directory across parents and over an empty one", seq_rename_dir, 1},
    {"link", seq_link, 1},
    {"symlink", seq_symlink, 0},
    {"an open unlinked file", seq_open_unlinked, 0},
    {"many files", seq_many, 0},
};

/* What each finished sequence leaves: the files it made with their bytes,
   the names that must be gone, and the base files untouched. */
static int base_intact(void)
{
    unsigned char want[8];

    __builtin_memcpy(want, "hello\n", 6);
    return file_is("/hello.txt", want, 6) &&
           file_is("/dir/sub/deep.txt", (const unsigned char *)"deep\n", 5);
}

static int oracle_after(int i)
{
    int err;
    void *n;
    struct vfs_stat st;

    if (!base_intact()) {
        return 0;
    }
    switch (i) {
    case 0:
        return file_is("/new.txt", pattern, 100) == 0 ? 0 : 1;
    case 1:
        return file_is("/big.bin", pattern, 100000);
    case 2: {
        unsigned char want[100016];

        __builtin_memset(want, 0, 100000);
        __builtin_memcpy(want + 100000, pattern, 16);
        return file_is("/holey.bin", want, 100016);
    }
    case 3: {
        unsigned char want[7000];

        __builtin_memcpy(want, pattern, 5000);
        __builtin_memset(want + 5000, 0, 2000);
        return file_is("/cut.bin", want, 7000);
    }
    case 4:
        return walk("/gone.txt", &err) == 0 && err == -ENOENT;
    case 5:
        n = walk("/newdir/inner", &err);
        if (!n) {
            return 0;
        }
        OPS->put(&mnt, n);
        return walk("/olddir", &err) == 0 && err == -ENOENT;
    case 6:
        return file_is("/dir/moved.txt", pattern, 3000) && walk("/from.txt", &err) == 0 &&
               walk("/to.txt", &err) == 0;
    case 7:
        return file_is("/victim.txt", pattern, 2000) && walk("/src.txt", &err) == 0;
    case 8:
        return file_is("/empty/inside.txt", pattern, 500) && walk("/mover", &err) == 0 &&
               walk("/dir/sub/moved", &err) == 0;
    case 9:
        n = walk("/two.txt", &err);
        if (!n || OPS->getattr(&mnt, n, &st) != 0 || st.nlink != 1) {
            if (n) {
                OPS->put(&mnt, n);
            }
            return 0;
        }
        OPS->put(&mnt, n);
        return file_is("/two.txt", pattern, 700) && walk("/one.txt", &err) == 0;
    case 10: {
        char buf[300];

        n = walk("/quick", &err);
        if (!n) {
            return 0;
        }
        err = OPS->readlink(&mnt, n, buf, sizeof(buf));
        OPS->put(&mnt, n);
        if (err != 9 || memcmp(buf, "hello.txt", 9) != 0) {
            return 0;
        }
        n = walk("/lengthy", &err);
        if (!n) {
            return 0;
        }
        err = OPS->readlink(&mnt, n, buf, sizeof(buf));
        OPS->put(&mnt, n);
        return err == 199 && buf[0] == 't' && buf[198] == 't';
    }
    case 11:
        return walk("/temp.txt", &err) == 0 && err == -ENOENT;
    case 12:
        return file_is("/m1.txt", pattern, 10) && file_is("/m299.txt", pattern, 10) &&
               walk("/m0.txt", &err) == 0 && walk("/m298.txt", &err) == 0;
    default:
        return 0;
    }
}

/* A sequence's own case 0 above compares only the first hundred bytes;
   the whole file is 10100 bytes of the pattern from the start, twice. */
static int oracle_create_write(void)
{
    unsigned char want[10100];

    __builtin_memcpy(want, pattern, 100);
    __builtin_memcpy(want + 100, pattern, 10000);
    return file_is("/new.txt", want, 10100);
}

static void write_rows(void)
{
    int i;

    fill_pattern();
    for (i = 0; i < (int)(sizeof(sequences) / sizeof(sequences[0])); i++) {
        char what[200];
        int rc;

        wbytes = build_image(wimg, 4096, 128, PROFILE, 16, "tree");
        use_image(wimg, wbytes);
        stop_after = -1;
        writes_made = 0;
        rc = mount_rw();
        snprintf(what, sizeof(what), "'%s': the volume mounts for writing", sequences[i].name);
        check(rc == 0 && (mnt.flags & VFS_MS_RDONLY) == 0, what);
        if (rc != 0) {
            continue;
        }
        rc = sequences[i].run();
        snprintf(what, sizeof(what), "'%s' runs", sequences[i].name);
        check(rc == 0, what);
        if (rc != 0) {
            printf("  rc %d\n", rc);
        }
        snprintf(what, sizeof(what), "'%s' reads back through the driver", sequences[i].name);
        check(i == 0 ? oracle_create_write() : oracle_after(i), what);
        rc = unmount_rw();
        snprintf(what, sizeof(what), "'%s' unmounts", sequences[i].name);
        check(rc == 0, what);
        snprintf(what, sizeof(what), "'%s' leaves a volume e2fsck calls clean", sequences[i].name);
        check(e2fsck_clean(wimg), what);
        if (!e2fsck_clean(wimg)) {
            system("head -20 " FIXDIR "/fsck.out");
        }
        snprintf(what, sizeof(what), "'%s' leaves a volume the reader walks", sequences[i].name);
        check(reader_walks(wimg), what);
        /* And on the next mount: the valid bit back, every byte the same. */
        use_image(wimg, wbytes);
        check(mount_image() == 0 && (mnt.flags & VFS_MS_RDONLY) == 0,
              "the next mount finds the volume clean and writable");
        snprintf(what, sizeof(what), "'%s' reads back after a remount", sequences[i].name);
        check(i == 0 ? oracle_create_write() : oracle_after(i), what);
        unmount_image();
    }
    /* The superblock's counts and state: the valid bit cleared by the
       mount, the counts untouched by a sequence, both written at unmount. */
    wbytes = build_image(wimg, 4096, 128, PROFILE, 16, "tree");
    use_image(wimg, wbytes);
    if (mount_rw() == 0) {
        unsigned char sb[16], sb2[16];

        host_read(wimg, sb, 16, 1024 + 12); /* the counts and, at 1082, the state */
        check((host_read(wimg, sb2, 2, 1024 + 58) == 2) && (sb2[0] & 1) == 0,
              "a mount for writing clears the valid bit before it is published");
        seq_create_write();
        seq_mkdir_rmdir();
        host_read(wimg, sb2, 16, 1024 + 12);
        check(memcmp(sb, sb2, 8) == 0,
              "a sequence leaves the superblock's free counts as they were");
        OPS->sync(&mnt);
        host_read(wimg, sb2, 16, 1024 + 12);
        check(memcmp(sb, sb2, 8) != 0, "a sync writes the counts");
        unmount_rw();
        host_read(wimg, sb2, 2, 1024 + 58);
        check((sb2[0] & 1) == 1, "an unmount sets the valid bit again");
    }

    /* debugfs lists what this driver wrote. */
    wbytes = build_image(wimg, 4096, 128, PROFILE, 16, "tree");
    use_image(wimg, wbytes);
    if (mount_rw() == 0) {
        seq_create_write();
        seq_mkdir_rmdir();
        seq_symlink();
        unmount_rw();
        check(debugfs_lists(wimg, "/", "new.txt") && debugfs_lists(wimg, "/newdir", "inner") &&
                  debugfs_lists(wimg, "/", "quick"),
              "debugfs lists the file, the directory and the link this driver wrote");
    }
}

/* The refusals the writer must decide before it writes anything: a name
   of 256 bytes, a write past the kernel's offset, the reserved blocks
   refused to a caller who is not root, and the volume byte for byte as
   it was after each. */
static unsigned char snap[1 << 24];

static int image_equals_snap(const char *img, uint64_t bytes)
{
    void *f = fopen(img, "rb");
    uint64_t at = 0;

    if (!f) {
        return 0;
    }
    while (at < bytes) {
        unsigned long take = bytes - at < 65536 ? (unsigned long)(bytes - at) : 65536;

        if (fread(bbuf, 1, take, f) != take || memcmp(bbuf, snap + at, take) != 0) {
            fclose(f);
            return 0;
        }
        at += take;
    }
    fclose(f);
    return 1;
}

static void snapshot(const char *img, uint64_t bytes)
{
    void *f = fopen(img, "rb");

    if (f) {
        fread(snap, 1, (unsigned long)bytes, f);
        fclose(f);
    }
}

static void refusal_write_rows(void)
{
    char name[300];
    int i, rc, err;
    void *n;
    void *h;

    wbytes = build_image(wimg, 4096, 128, PROFILE, 4, "tree");
    use_image(wimg, wbytes);
    check(mount_rw() == 0, "the small volume mounts for writing");
    if (!mnt.root) {
        return;
    }
    bcache_sync(&dev);
    snapshot(wimg, wbytes);
    for (i = 0; i < 256; i++) {
        name[i] = 'n';
    }
    name[256] = '\0';
    rc = OPS->create(&mnt, mnt.root, name, 0644, &n);
    check(rc == -ENAMETOOLONG, "a name of 256 bytes answers ENAMETOOLONG");
    bcache_sync(&dev);
    check(image_equals_snap(wimg, wbytes), "and the volume is as it was");
    name[255] = '\0';
    n = make_file(mnt.root, name, &rc);
    check(n != 0, "a name of 255 bytes is made");
    if (n) {
        OPS->put(&mnt, n);
    }
    n = walk("/hello.txt", &err);
    if (n) {
        bcache_sync(&dev);
        snapshot(wimg, wbytes);
        check(write_at(n, pattern, 1, 0xFFFFFFFFull) == -EFBIG,
              "a write past the kernel's offset answers EFBIG");
        bcache_sync(&dev);
        check(image_equals_snap(wimg, wbytes), "and the volume is as it was");
        OPS->put(&mnt, n);
    }
    /* The reserved blocks: a caller who is not root stops short of them,
       and root does not. */
    proc_a.euid = 1000;
    n = make_file(mnt.root, "filler.bin", &rc);
    if (n) {
        uint64_t off = 0;
        int last = 0;

        while (off < 8u * 1024 * 1024) {
            last = write_at(n, pattern, 4096, off);
            if (last != 0) {
                break;
            }
            off += 4096;
        }
        check(last == -ENOSPC, "a caller who is not root answers ENOSPC at the reserve");
        proc_a.euid = 0;
        rc = write_at(n, pattern, 4096, off);
        if (rc != 0) {
            printf("  root's write: %d (%s)\n", rc, last_warn);
        }
        check(rc == 0, "and root writes into it");
        OPS->put(&mnt, n);
    }
    proc_a.euid = 0;
    /* The sixty-fourth node held and the sixty-fifth refused, over a
       file each. */
    {
        void *held[EXT2_NODES];
        int took = 0;

        for (i = 0; i < EXT2_NODES; i++) {
            char nm[32];

            snprintf(nm, sizeof(nm), "p%d.txt", i);
            n = make_file(mnt.root, nm, &rc);
            if (!n) {
                break;
            }
            held[took++] = n;
        }
        check(took == EXT2_NODES - 1 && rc == -ENOMEM,
              "the sixty-fourth node is the root's neighbour and the next answers ENOMEM");
        for (i = 0; i < took; i++) {
            OPS->put(&mnt, held[i]);
        }
    }
    rc = OPS->open(&mnt, mnt.root, 0, &h);
    if (rc == 0) {
        OPS->release(&mnt, h);
    }
    check(unmount_rw() == 0, "the small volume unmounts");
    check(e2fsck_clean(wimg), "and e2fsck calls it clean");
}

/* ---- the interruption matrix ---- */

/* The matrix's volume: the fixture with every free block full of bytes
   that would read as data, so that a size published over bytes not yet
   synced reads junk and not the zeros a fresh image has. */
static int copy_file(const char *src, const char *dst);

static uint64_t build_junk_image(void)
{
    static uint64_t bytes;

    if (bytes == 0) {
        char cmd[700];

        unsigned char probe[64];
        int i, nonzero = 0;

        bytes = build_image(FIXDIR "/junk.ext2", 4096, 128, PROFILE, 16, "tree");
        snprintf(cmd, sizeof(cmd),
                 ". tools/setup/e2fsprogs_path.sh; head -c 14000000 /dev/urandom > " FIXDIR
                 "/junk.bin && "
                 "debugfs -w -R 'write " FIXDIR "/junk.bin junk' " FIXDIR
                 "/junk.ext2 >/dev/null 2>&1 && "
                 "debugfs -w -R 'rm junk' " FIXDIR "/junk.ext2 >/dev/null 2>&1");
        if (!run(cmd)) {
            printf("ext2_hosttest: the junk volume did not build\n");
            exit(1);
        }
        /* A free block well past the tree must hold the junk, or the matrix
           would run over zeros and its oracle would accept a size published
           before its data. */
        if (host_read(FIXDIR "/junk.ext2", probe, sizeof(probe), 3500L * 4096) !=
            (long)sizeof(probe)) {
            printf("ext2_hosttest: the junk volume did not read\n");
            exit(1);
        }
        for (i = 0; i < (int)sizeof(probe); i++) {
            nonzero |= probe[i] != 0;
        }
        if (!nonzero) {
            printf("ext2_hosttest: the junk volume's free blocks are zeros\n");
            exit(1);
        }
    }
    if (!copy_file(FIXDIR "/junk.ext2", FIXDIR "/w.ext2")) {
        printf("ext2_hosttest: the junk volume did not copy\n");
        exit(1);
    }
    return bytes;
}

/* A crashed sequence's file, when present, reads either the bytes it had
   before the sequence or the bytes it was to have after it, with a zero
   wherever a byte was not yet synced; anything else is junk that a size
   was published over. */
static int crash_bytes_ok(const char *path, const unsigned char *before, uint32_t blen,
                          const unsigned char *after, uint32_t alen)
{
    int err, got;
    void *n = walk(path, &err);
    uint32_t i;
    int as_before = 1, as_after = 1;

    if (!n) {
        return 1; /* absent is a state before the name was written */
    }
    got = read_at(n, abuf, sizeof(abuf), 0);
    OPS->put(&mnt, n);
    if (got < 0) {
        return 0;
    }
    if (before == 0 || (uint32_t)got != blen) {
        as_before = 0;
    }
    for (i = 0; as_before && i < (uint32_t)got; i++) {
        if (abuf[i] != before[i]) {
            as_before = 0;
        }
    }
    if ((uint32_t)got > alen) {
        as_after = 0;
    }
    for (i = 0; as_after && i < (uint32_t)got; i++) {
        if (abuf[i] != after[i] && abuf[i] != 0) {
            as_after = 0;
        }
    }
    if (!as_before && !as_after) {
        printf("  %s: %d bytes that are neither before nor after\n", path, got);
    }
    return as_before || as_after;
}

static int crash_ok(int i)
{
    static unsigned char want[300000];

    switch (i) {
    case 0:
        __builtin_memcpy(want, pattern, 100);
        __builtin_memcpy(want + 100, pattern, 10000);
        return crash_bytes_ok("/new.txt", 0, 0, want, 10100);
    case 1:
        return crash_bytes_ok("/big.bin", 0, 0, pattern, 100000);
    case 2:
        __builtin_memset(want, 0, 100000);
        __builtin_memcpy(want + 100000, pattern, 16);
        return crash_bytes_ok("/holey.bin", 0, 0, want, 100016);
    case 3:
        /* Written to 40000, cut to 5000, grown to 7000: the long form
           is a state too. */
        __builtin_memcpy(want, pattern, 5000);
        __builtin_memset(want + 5000, 0, 2000);
        return crash_bytes_ok("/cut.bin", pattern, 40000, want, 7000);
    case 4:
        return crash_bytes_ok("/gone.txt", 0, 0, pattern, 12000);
    case 6:
        return crash_bytes_ok("/from.txt", 0, 0, pattern, 3000) &&
               crash_bytes_ok("/to.txt", 0, 0, pattern, 3000) &&
               crash_bytes_ok("/dir/moved.txt", 0, 0, pattern, 3000);
    case 7:
        return crash_bytes_ok("/src.txt", 0, 0, pattern, 2000) &&
               crash_bytes_ok("/victim.txt", pattern + 100, 9000, pattern, 2000);
    case 8:
        return crash_bytes_ok("/mover/inside.txt", 0, 0, pattern, 500) &&
               crash_bytes_ok("/dir/sub/moved/inside.txt", 0, 0, pattern, 500) &&
               crash_bytes_ok("/empty/inside.txt", 0, 0, pattern, 500);
    case 9:
        return crash_bytes_ok("/one.txt", 0, 0, pattern, 700) &&
               crash_bytes_ok("/two.txt", 0, 0, pattern, 700);
    case 11:
        return crash_bytes_ok("/temp.txt", 0, 0, pattern, 8000);
    default:
        return 1;
    }
}

/* A copy of `src` at `dst`, here rather than through cp: the matrix makes
   one per stop. */
static int copy_file(const char *src, const char *dst)
{
    void *in = fopen(src, "rb");
    void *out = in ? fopen(dst, "wb") : 0;
    unsigned long got;
    int ok = out != 0;

    while (ok && (got = fread(abuf, 1, sizeof(abuf), in)) > 0) {
        ok = fwrite(abuf, 1, got, out) == got;
    }
    if (in) {
        fclose(in);
    }
    if (out) {
        fclose(out);
    }
    return ok;
}

/* The check over a copy of the crash state: a writable mount, or a
   read-only one whose fault is what the order writes in more than one
   step: a link count after its entry, a size and an i_blocks after the
   pointers, and a directory's two entries and its `..` across a move. */
static int check_accepts(const char *img, uint64_t bytes, int counts_written)
{
    static const char *const counts[] = {
        "a link count that does not match its names",
        "a directory link count that does not match its entries",
        "named twice with one link",
        "a directory with two parents",
        "a second entry that is not the parent",
    };
    static const char *const sizes[] = {
        "a block past the size",
        "a pointer block past the size",
        "i_blocks that does not match its pointers",
    };
    int ok, i;

    if (!copy_file(img, FIXDIR "/check.ext2")) {
        return 0;
    }
    use_image(FIXDIR "/check.ext2", bytes);
    last_check[0] = 0;
    if (mount_image() != 0) {
        printf("  the check refused the stop: %s\n", last_warn);
        return 0;
    }
    ok = (mnt.flags & VFS_MS_RDONLY) == 0;
    for (i = 0; !ok && i < 3; i++) {
        ok = strstr(last_check, sizes[i]) != 0;
    }
    for (i = 0; !ok && counts_written && i < 5; i++) {
        ok = strstr(last_check, counts[i]) != 0;
    }
    if (!ok) {
        printf("  the check found: %s\n", last_check);
    }
    unmount_image();
    return ok;
}

/* Whether the sequence's moved or linked file is reachable by any of its
   names on the mounted volume. */
static int names_present(int i)
{
    int err;

    switch (i) {
    case 0:
        return walk("/new.txt", &err) != 0;
    case 5:
        return walk("/newdir", &err) != 0;
    case 10:
        return walk("/quick", &err) != 0;
    case 6:
        return walk("/from.txt", &err) || walk("/to.txt", &err) || walk("/dir/moved.txt", &err);
    case 7:
        return walk("/src.txt", &err) || walk("/victim.txt", &err);
    case 8:
        return walk("/mover/inside.txt", &err) || walk("/dir/sub/moved/inside.txt", &err) ||
               walk("/empty/inside.txt", &err);
    case 9:
        return walk("/one.txt", &err) || walk("/two.txt", &err);
    default:
        return 0;
    }
}

/* The judges of a stop: the volume mounts and every base file reads;
   e2fsck -fn runs, clean or with faults it names; e2fsck -fy on a copy
   repairs it, and the repaired copy mounts with every base file intact
   and the sequence's files, where present, readable. */
static int repaired_ok(const char *img, uint64_t bytes, int i, int stop, int named_before)
{
    char cmd[700];
    int rc;

    snprintf(cmd, sizeof(cmd),
             ". tools/setup/e2fsprogs_path.sh; cp %s " FIXDIR "/repair.ext2 && e2fsck -fy " FIXDIR
             "/repair.ext2 > " FIXDIR "/repair.out 2>&1; s=$?; test $s -le 2",
             img);
    if (system(cmd) != 0) {
        printf("  e2fsck -fy could not repair '%s' stopped after write %d\n", sequences[i].name,
               stop);
        return 0;
    }
    use_image(FIXDIR "/repair.ext2", bytes);
    rc = mount_image();
    if (rc != 0) {
        return 0;
    }
    rc = base_intact();
    /* A file reachable by a name in the crash state keeps a name after the
       repair: what the order of writes promises for rename and link. */
    if (rc && named_before && !names_present(i)) {
        printf("  '%s' stopped after write %d: the file lost every name\n", sequences[i].name,
               stop);
        rc = 0;
    }
    unmount_image();
    return rc;
}

/* ---- the dirty-volume rows ---- */

/* Run debugfs -w over `img` with the requests in `script`, one per line. */
static int debugfs_script(const char *img, const char *script)
{
    char cmd[512];
    void *f = fopen(FIXDIR "/dbg.cmd", "wb");

    if (!f) {
        return 0;
    }
    fwrite(script, 1, strlen(script), f);
    fclose(f);
    snprintf(cmd, sizeof(cmd),
             ". tools/setup/e2fsprogs_path.sh; debugfs -w -f " FIXDIR "/dbg.cmd %s >/dev/null 2>&1",
             img);
    return run(cmd);
}

/* The number after `key` in debugfs's answer to `request`, or -1. */
static long debugfs_number(const char *img, const char *request, const char *key)
{
    char cmd[512], line[512];
    void *f;
    long v = -1;

    snprintf(cmd, sizeof(cmd),
             ". tools/setup/e2fsprogs_path.sh; debugfs -R '%s' %s 2>/dev/null > " FIXDIR "/dbg.out",
             request, img);
    if (!run(cmd)) {
        return -1;
    }
    f = fopen(FIXDIR "/dbg.out", "rb");
    if (!f) {
        return -1;
    }
    while (fgets(line, sizeof(line), f)) {
        char *at = strstr(line, key);

        if (at && at[strlen(key)] >= '0' && at[strlen(key)] <= '9') {
            v = atol(at + strlen(key));
            break;
        }
    }
    fclose(f);
    return v;
}

/* The superblock's state field on disk. */
static int disk_state(const char *img)
{
    unsigned char b[2];

    if (host_read(img, b, 2, 1024 + 58) != 2) {
        return -1;
    }
    return b[0] | (b[1] << 8);
}

/* Write `/fill` until the volume is full; 0 when ENOSPC ended it. */
static int fill_volume(void)
{
    int rc;
    void *n = make_file(mnt.root, "fill", &rc);
    uint64_t off = 0;

    if (!n) {
        return rc;
    }
    for (;;) {
        rc = write_at(n, pattern, 4096, off);
        if (rc != 0) {
            break;
        }
        off += 4096;
    }
    OPS->put(&mnt, n);
    return rc == -ENOSPC ? 0 : rc;
}

/* A fresh tree volume with its valid bit clear and `script` applied. */
static uint64_t dirty_image(const char *img, const char *script)
{
    uint64_t bytes = build_image(img, 4096, 128, PROFILE, 16, "tree");
    char cmd[1024];

    snprintf(cmd, sizeof(cmd), "ssv state 0\n%s", script);
    if (!debugfs_script(img, cmd)) {
        printf("ext2_hosttest: debugfs failed on %s\n", img);
        exit(1);
    }
    return bytes;
}

/* A dirty volume that must mount read-only with `fault` named by the check. */
static void fault_row(const char *img, const char *script, const char *fault, const char *what)
{
    uint64_t bytes = dirty_image(img, script);
    int rc;

    use_image(img, bytes);
    last_check[0] = 0;
    rc = mount_rw();
    if (rc != 0 || (mnt.flags & VFS_MS_RDONLY) == 0 || !strstr(last_check, fault)) {
        printf("  mount %d, flags 0x%x, check: %s\n", rc, (unsigned)mnt.flags, last_check);
        check(0, what);
    } else {
        void *n = make_file(mnt.root, "new", &rc);

        check(n == 0 && rc == -EROFS && (disk_state(img) & 2) != 0,
              "and refuses a create with EROFS, the error bit set on disk");
        check(1, what);
    }
    if (rc == 0 || mnt.root) {
        unmount_rw();
    }
}

static void dirty_rows(void)
{
    const char *img = FIXDIR "/dirty.ext2";
    uint64_t bytes;
    unsigned char b[4];
    char script[256];
    long blk, ino, table, root_block;
    struct vfs_statfs st;
    int rc;

    /* A consistent volume with its valid bit clear: checked, writable,
       valid again at unmount. */
    bytes = dirty_image(img, "");
    use_image(img, bytes);
    last_check[0] = 0;
    check(mount_rw() == 0 && (mnt.flags & VFS_MS_RDONLY) == 0 && streq(last_check, "check: clean"),
          "a consistent volume whose valid bit is clear is checked and mounts writable");
    rc = unmount_rw();
    check(rc == 0 && disk_state(img) == 1 && e2fsck_clean(img),
          "and its unmount marks it valid again");

    /* The counts are recounted, not believed. */
    bytes = dirty_image(img, "ssv free_blocks_count 1\nssv free_inodes_count 1\n"
                             "set_bg 0 free_blocks_count 1\nset_bg 0 free_inodes_count 1\n");
    use_image(img, bytes);
    check(mount_rw() == 0 && OPS->statfs(&mnt, &st) == 0 && st.bfree > 1000,
          "a dirty volume's free counts are recounted from the bitmaps");
    check(unmount_rw() == 0 && e2fsck_clean(img), "and written back, so e2fsck finds them right");

    /* A lost block is kept, out of the allocator's reach, and the error
       bit says so until e2fsck has run. */
    bytes = dirty_image(img, "setb 4000\n");
    use_image(img, bytes);
    last_check[0] = 0;
    check(mount_rw() == 0 && (mnt.flags & VFS_MS_RDONLY) == 0 &&
              strstr(last_check, "1 lost blocks, 0 orphan inodes") != 0,
          "a lost block mounts writable with the block counted");
    check(fill_volume() == 0 && unmount_rw() == 0 && disk_state(img) == 2 &&
              e2fsck_status(img) == 4 && run("grep -q -- '-(4000)\\|-4000' " FIXDIR "/fsck.out"),
          "and filling the volume leaves it lost: the error bit set, e2fsck still owed it");
    use_image(img, bytes);
    last_check[0] = 0;
    check(mount_rw() == 0 && strstr(last_check, "1 lost blocks") != 0 && unmount_rw() == 0 &&
              disk_state(img) == 2,
          "the error bit survives a mount and an unmount");
    snprintf(script, sizeof(script),
             ". tools/setup/e2fsprogs_path.sh; e2fsck -fp %s >/dev/null 2>&1; test $? -eq 1", img);
    check(run(script), "e2fsck -p repairs it");
    use_image(img, bytes);
    last_check[0] = 0;
    check(mount_rw() == 0 && (mnt.flags & VFS_MS_RDONLY) == 0 && last_check[0] == 0 &&
              unmount_rw() == 0 && disk_state(img) == 1,
          "and the repaired volume mounts without a check and unmounts valid");

    /* An orphan with its blocks is kept whole. */
    bytes = build_image(img, 4096, 128, PROFILE, 16, "tree");
    ino = debugfs_number(img, "stat /dir/big.bin", "Inode: ");
    blk = debugfs_number(img, "blocks /dir/big.bin", "");
    bytes = dirty_image(img, "unlink /dir/big.bin\n");
    use_image(img, bytes);
    last_check[0] = 0;
    check(ino > 0 && blk > 0 && mount_rw() == 0 && (mnt.flags & VFS_MS_RDONLY) == 0 &&
              strstr(last_check, "0 lost blocks, 1 orphan inodes") != 0,
          "an orphan inode mounts writable with the orphan counted");
    snprintf(script, sizeof(script),
             "grep -q 'Unattached inode %ld' " FIXDIR "/fsck.out && ! grep -q 'Multiply' " FIXDIR
             "/fsck.out",
             ino);
    check(fill_volume() == 0 && unmount_rw() == 0 && e2fsck_status(img) == 4 && run(script),
          "and filling the volume leaves the orphan and its blocks to e2fsck");

    /* The faults, each read-only by name. */
    bytes = build_image(img, 4096, 128, PROFILE, 16, "tree");
    blk = debugfs_number(img, "blocks /hello.txt", "");
    snprintf(script, sizeof(script), "unlink /hello.txt\nunlink /hello.link\nfreeb %ld\n", blk);
    fault_row(img, script, "a used block the bitmap calls free",
              "an orphan whose block the bitmap calls free mounts read-only");
    if (host_read(img, b, 4, 4096 + 8) == 4) {
        table = b[0] | (b[1] << 8) | (b[2] << 16) | ((long)b[3] << 24);
        snprintf(script, sizeof(script), "sif /dir/big.bin block[0] %ld\n", table);
        fault_row(img, script, "a block claimed twice",
                  "a file pointing into the inode table mounts read-only");
    } else {
        check(0, "the descriptor read for the inode-table row");
    }
    fault_row(img, "freei /dir/big.bin\n", "a reached inode the bitmap calls free",
              "a reached inode whose bit is clear mounts read-only");
    fault_row(img, "sif /dir/big.bin links_count 3\n", "a link count that does not match its names",
              "a link count above its names mounts read-only");
    fault_row(img, "sif /dir links_count 9\n",
              "a directory link count that does not match its entries",
              "a directory count above its entries mounts read-only");
    fault_row(img, "link /dir/sub /many/twice\nsif /many links_count 3\n",
              "a directory with two parents", "a directory named twice mounts read-only");
    fault_row(img, "sif /dir/big.bin size 100\n", "past the size",
              "a size behind its pointers mounts read-only");
    fault_row(img, "sif /dir/big.bin i_blocks 8\n", "i_blocks that does not match its pointers",
              "an i_blocks behind its pointers mounts read-only");
    bytes = build_image(img, 4096, 128, PROFILE, 16, "tree");
    root_block = debugfs_number(img, "blocks /", "");
    if (root_block > 0) {
        b[0] = 6;
        b[1] = 0;
        bytes = dirty_image(img, "");
        poke(img, root_block * 4096 + 4, b, 2);
        use_image(img, bytes);
        last_check[0] = 0;
        check(mount_rw() == 0 && (mnt.flags & VFS_MS_RDONLY) != 0 &&
                  strstr(last_check, "an entry of the wrong form") != 0,
              "an entry of the wrong form mounts read-only");
        unmount_rw();
    } else {
        check(0, "the root's block for the entry-form row");
    }

    /* A read-only mount asks for no check and writes nothing. */
    bytes = dirty_image(img, "setb 4000\n");
    snapshot(img, bytes);
    use_image(img, bytes);
    last_check[0] = 0;
    check(mount_ro() == 0 && last_check[0] == 0,
          "a read-only mount of a dirty volume is not checked");
    unmount_image();
    check(image_equals_snap(img, bytes), "and writes nothing");
}

static void matrix_rows(void)
{
    int i, k, total = 0;

    for (i = 0; i < (int)(sizeof(sequences) / sizeof(sequences[0])); i++) {
        int n, mark;

        if (i == 12) {
            continue; /* many files: thousands of stops; the others cover its writes */
        }
        wbytes = build_image(wimg, 4096, 128, PROFILE, 16, "tree");
        use_image(wimg, wbytes);
        stop_after = -1;
        if (mount_rw() != 0) {
            check(0, "the matrix's volume mounts");
            return;
        }
        writes_made = 0; /* the mount's own writes are not the sequence's */
        op_mark = -1;
        sequences[i].run();
        n = writes_made;
        mark = op_mark;
        unmount_rw();
        for (k = 1; k <= n; k++) {
            char what[200];
            int status, named_before = 0;

            wbytes = build_junk_image();
            use_image(wimg, wbytes);
            stop_after = -1;
            if (mount_rw() != 0) {
                printf("  stop %d of '%s': %s\n", k, sequences[i].name, last_warn);
                check(0, "the matrix's volume mounts again");
                return;
            }
            writes_made = 0;
            stop_after = k;
            sequences[i].run();
            /* The machine stops: what the cache still holds is dropped as
               a gone device's buffers are, so the image is the crash state
               and nothing unsynced reaches it. */
            OPS->put(&mnt, mnt.root);
            OPS->unmount(&mnt);
            mnt.root = 0;
            bcache_device_gone(&dev);
            dev.flags = BLOCK_FAULT_SAFE;
            stop_after = -1;
            /* Judge one: the volume mounts, the base files read, and the
               sequence's files read only what was written or zeros. */
            use_image(wimg, wbytes);
            snprintf(what, sizeof(what),
                     "'%s' stopped after write %d of %d mounts and reads its base",
                     sequences[i].name, k, n);
            if (mount_ro() == 0) {
                check(base_intact(), what);
                snprintf(what, sizeof(what),
                         "'%s' stopped after write %d of %d reads only what was written or zeros",
                         sequences[i].name, k, n);
                check(crash_ok(i), what);
                named_before = names_present(i);
                if (mark >= 0 && k > mark) {
                    /* Past the start of the rename or link, or the commit of
                       a new name, the file has a name at every stop: what
                       the order of writes is for. */
                    snprintf(what, sizeof(what),
                             "'%s' stopped after write %d of %d keeps a name for its file",
                             sequences[i].name, k, n);
                    check(named_before, what);
                    named_before = 1;
                }
                unmount_image();
            } else {
                check(0, what);
            }
            /* Judge two: e2fsck names the state, clean or repairable. */
            status = e2fsck_status(wimg);
            snprintf(what, sizeof(what),
                     "'%s' stopped after write %d of %d is a state e2fsck can judge",
                     sequences[i].name, k, n);
            if (status != 0 && status != 4) {
                printf("  e2fsck -fn exited %d:\n", status);
                system("head -14 " FIXDIR "/fsck.out");
            }
            check((status == 0 || status == 4) && findings_allowed(sequences[i].counts), what);
            /* Judge three: repaired on a copy, nothing outside the operation
               lost. */
            snprintf(what, sizeof(what),
                     "'%s' stopped after write %d of %d repairs with its base intact",
                     sequences[i].name, k, n);
            check(repaired_ok(wimg, wbytes, i, k, named_before), what);
            /* Judge four: the mount-time check accepts the stop, on a copy:
               writable with what the order leaves kept, or read-only on a
               fault the order leaves between two of its writes, a count
               behind its pointers or its entry, a directory between its
               two parents. */
            snprintf(what, sizeof(what),
                     "'%s' stopped after write %d of %d passes the check or fails it on a count",
                     sequences[i].name, k, n);
            check(check_accepts(wimg, wbytes, sequences[i].counts), what);
            total++;
        }
    }
    printf("ext2_hosttest: %d stops walked\n", total);
    /* The comparison looks: a base file's block corrupted in a repaired
       copy fails it. */
    wbytes = build_image(wimg, 4096, 128, PROFILE, 16, "tree");
    {
        char cmd[512];

        snprintf(cmd, sizeof(cmd),
                 ". tools/setup/e2fsprogs_path.sh; debugfs -w -R 'write /dev/null hello.txt' %s "
                 ">/dev/null 2>&1; "
                 "debugfs -w -R 'set_inode_field /hello.txt size 5' %s >/dev/null 2>&1",
                 wimg, wimg);
        system(cmd);
    }
    use_image(wimg, wbytes);
    if (mount_image() == 0) {
        check(!base_intact(), "a base file changed under the repair is seen by the comparison");
        unmount_image();
    }
}

/* One instance at a time over the fixture directory, which is one name for
   every run: the lock is a file made exclusively, removed at exit. */
static void unlock_fixtures(void)
{
    unlink(FIXDIR ".lock");
}

static void lock_fixtures(void)
{
    int i, fd;

    for (i = 0; i < 600; i++) {
        fd = open(FIXDIR ".lock", 0x0200 | 0x0800 | 0x0001, 0644); /* O_CREAT|O_EXCL|O_WRONLY */
        if (fd >= 0) {
            close(fd);
            atexit(unlock_fixtures);
            return;
        }
        sleep(1);
    }
    printf("ext2_hosttest: another instance holds " FIXDIR ".lock\n");
    exit(1);
}

int main(void)
{
    uint64_t bytes;

    if (!run("mkdir -p /tmp/rach2_verify")) {
        printf("ext2_hosttest: /tmp/rach2_verify did not appear\n");
        return 1;
    }
    lock_fixtures();
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
    write_rows();
    refusal_write_rows();
    dirty_rows();
    matrix_rows();

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
