/*
 * exfat_hosttest.c: the exfat driver over the buffer cache over an image.
 *
 * host-deps: rach2/sys/fs/exfat.c rach2/sys/fs/bcache.c
 * host-deps: rach2/sys/fs/fatcommon.c
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 *
 * The volumes are mkexfatfixture's, built here on every run. The
 * well-formed volume must read back what the builder wrote, through every
 * shape: the contiguous file, the file chained over clusters that are not
 * adjacent, the file whose tail past ValidDataLength reads as zeros, the
 * empty file, the name of four entries, the set with vendor entries, the
 * set that straddles a sector, the set that crosses a cluster boundary,
 * and the directory of two levels; a name is found by any case through
 * the volume's up-case table, and in each of the table's three forms.
 * Each malformed variant is refused at mount with the field named, mounts
 * read-only with the reason named, has its set skipped, or fails at the
 * file it damages. The gate: a volume the host's own tools formatted and
 * filled mounts and lists every file in its manifest with the right bytes
 * and times.
 */

#include "../../rach2/sys/fs/exfat.h"
#include "../../rach2/sys/fs/bcache.h"
#include "../../rach2/sys/lib/kerrno.h"
#include "../../rach2/sys/proc/process.h"

/* No hosted headers: the kernel headers define the fixed-width types. */
extern int printf(const char *fmt, ...);
extern int snprintf(char *s, unsigned long n, const char *fmt, ...);
extern int system(const char *cmd);
extern void exit(int status);
extern void *fopen(const char *path, const char *mode);
extern unsigned long fread(void *buf, unsigned long size, unsigned long n, void *f);
extern char *fgets(char *s, int n, void *f);
extern int fclose(void *f);
extern unsigned long strlen(const char *s);
extern char *strchr(const char *s, int c);
extern char *strstr(const char *hay, const char *needle);
extern char *strcat(char *dst, const char *src);
extern int memcmp(const void *a, const void *b, unsigned long n);
extern void *memcpy(void *dst, const void *src, unsigned long n);
extern void *memset(void *dst, int c, unsigned long n);
extern long atol(const char *s);

#define FIXDIR    "/tmp/rach2_verify"
#define IMG_BYTES (65536u * 512u) /* 32 MiB: the fixture is 16, the external volume 16 */

/* ---- what the driver needs of the kernel ---- */

static process_t proc_a;
process_t *current_proc = &proc_a;

/* The kernel's records go to the log the runner keeps, so a refusal or a
   skipped set names its reason there. */
extern int vprintf(const char *fmt, __builtin_va_list ap);
void klog(int level, const char *subsystem, const char *fmt, ...)
{
    __builtin_va_list ap;

    (void)level;
    printf("klog: %s: ", subsystem);
    __builtin_va_start(ap, fmt);
    vprintf(fmt, ap);
    __builtin_va_end(ap);
    printf("\n");
}
void k_panic(const char *msg)
{
    printf("exfat_hosttest: panic: %s\n", msg);
    exit(1);
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
    printf("exfat_hosttest: FAIL: a row reached the sleeping wait\n");
    exit(1);
}
int wq_wait_event_nointr(wait_queue_t *wq, wq_cond_fn cond, void *arg)
{
    return wq_wait_event(wq, cond, arg);
}
void rtc_realtime(int64_t *seconds, int64_t *nanoseconds)
{
    *seconds = 1788264000; /* 2026-09-01 12:00:00 UTC */
    *nanoseconds = 0;
}
void vfs_device_failed(block_device_t *dev, uint64_t lba, int rc)
{
    (void)dev;
    (void)lba;
    (void)rc;
}
void vfs_ns_lock(struct vfs_mount *mnt)
{
    mnt->ns_holder = (int)current_proc->pid;
    mnt->ns_depth++;
}
void vfs_ns_unlock(struct vfs_mount *mnt)
{
    if (--mnt->ns_depth == 0) {
        mnt->ns_holder = -1;
    }
}

/* ---- the fake device ---- */

static unsigned char image[IMG_BYTES];
static uint64_t image_sectors;

static int img_read(block_device_t *d, uint64_t lba, uint32_t count, void *buf)
{
    (void)d;
    if (lba + count > image_sectors) {
        return -5;
    }
    __builtin_memcpy(buf, image + lba * 512, count * 512);
    return 0;
}

static int img_write(block_device_t *d, uint64_t lba, uint32_t count, const void *buf)
{
    (void)d;
    if (lba + count > image_sectors) {
        return -5;
    }
    __builtin_memcpy(image + lba * 512, buf, count * 512);
    return 0;
}

static block_device_t dev = {512, 32768u, BLOCK_FAULT_SAFE, 0, img_read, img_write, 0, 0};

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
        printf("exfat_hosttest: FAIL: %s\n", what);
    }
}

static int builder_built;

/* Load an image file into the device; its size is the device's. */
static int load_file(const char *path)
{
    void *f = fopen(path, "rb");
    unsigned long n;

    if (!f) {
        return -1;
    }
    bcache_sync(&dev);
    n = fread(image, 1, IMG_BYTES, f);
    fclose(f);
    if (n < 512 || n % 512 != 0) {
        return -1;
    }
    image_sectors = n / 512;
    dev.sector_count = image_sectors;
    if (bcache_invalidate(&dev) != 0) {
        printf("exfat_hosttest: the cache did not empty for a new image\n");
        exit(1);
    }
    return 0;
}

/** Build the named fixture and load it into the image. */
static void load(const char *opt)
{
    char cmd[256];

    if (!builder_built) {
        snprintf(cmd, sizeof(cmd), "cc -O2 -o %s/mkexfatfixture tests/host/mkexfatfixture.c",
                 FIXDIR);
        if (system(cmd) != 0) {
            printf("exfat_hosttest: cannot build the fixture builder\n");
            exit(1);
        }
        builder_built = 1;
    }
    snprintf(cmd, sizeof(cmd), "%s/mkexfatfixture %s/exfat_case.img %s", FIXDIR, FIXDIR, opt);
    if (system(cmd) != 0) {
        printf("exfat_hosttest: fixture builder failed for %s\n", opt);
        exit(1);
    }
    if (load_file(FIXDIR "/exfat_case.img") != 0) {
        printf("exfat_hosttest: fixture did not load\n");
        exit(1);
    }
}

static struct vfs_mount mnt;

#define OPS (vfs_exfat_type.ops)

/* Mount the loaded image with `flags` and `options`; the constructor's rc. */
static int mount_image(uint32_t flags, const char *options)
{
    __builtin_memset(&mnt, 0, sizeof(mnt));
    mnt.type = &vfs_exfat_type;
    mnt.dev = &dev;
    mnt.flags = flags;
    mnt.ns_holder = -1;
    return vfs_exfat_type.mount(&mnt, &dev, flags, options);
}

static void unmount_image(void)
{
    if (mnt.root) {
        OPS->put(&mnt, mnt.root);
        OPS->unmount(&mnt);
    }
}

/* Resolve a path from the root, one component at a time; the node held,
   or NULL with the errno in `*err`. */
static void *walk(const char *path, int *err)
{
    void *cur = mnt.root;
    char comp[300];

    if (!cur) {
        *err = -ENOENT; /* nothing mounted */
        return 0;
    }
    OPS->hold(&mnt, cur);
    while (*path) {
        const char *slash;
        unsigned long n;
        void *next;
        int rc;

        while (*path == '/') {
            path++;
        }
        if (!*path) {
            break;
        }
        slash = strchr(path, '/');
        n = slash ? (unsigned long)(slash - path) : strlen(path);
        memcpy(comp, path, n);
        comp[n] = '\0';
        rc = OPS->lookup(&mnt, cur, comp, &next);
        OPS->put(&mnt, cur);
        if (rc != 0) {
            *err = rc;
            return 0;
        }
        cur = next;
        path += n;
    }
    *err = 0;
    return cur;
}

static unsigned char back[262144];

/* Read a file whole through the driver: the byte count, or a negative rc. */
static long read_all(void *node, unsigned char *buf, uint32_t cap)
{
    void *h;
    uint32_t done = 0;

    if (OPS->open(&mnt, node, 0, &h) != 0) {
        return -1;
    }
    for (;;) {
        int32_t got = OPS->read(&mnt, h, buf + done, cap - done, done);

        if (got < 0) {
            OPS->release(&mnt, h);
            return got;
        }
        if (got == 0) {
            break;
        }
        done += (uint32_t)got;
        if (done == cap) {
            break;
        }
    }
    OPS->release(&mnt, h);
    return (long)done;
}

/* A damaged file: one read gives `sound` bytes, and the next answers EIO. */
static int read_then_fail(void *node, long sound)
{
    void *h;
    int32_t got, then;

    if (OPS->open(&mnt, node, 0, &h) != 0) {
        return 0;
    }
    got = OPS->read(&mnt, h, back, sizeof(back), 0);
    then = OPS->read(&mnt, h, back, sizeof(back), (uint64_t)sound);
    OPS->release(&mnt, h);
    return got == sound && then == -EIO;
}

/* The bytes of `path` equal `want` for `n` bytes, and the file is that long. */
static void check_bytes(const char *path, const unsigned char *want, long n, const char *what)
{
    int err;
    void *node = walk(path, &err);
    long got;

    if (!node) {
        check(0, what);
        return;
    }
    got = read_all(node, back, sizeof(back));
    OPS->put(&mnt, node);
    check(got == n && memcmp(back, want, (unsigned long)n) == 0, what);
}

/* Every name readdir lists in `dir`, joined by '|' into `out`. */
static void listing(void *dir, char *out, unsigned long cap)
{
    uint32_t cursor = 0;
    struct vfs_dirent ent;

    out[0] = '\0';
    while (OPS->readdir(&mnt, dir, &cursor, &ent) == 1) {
        unsigned long n = strlen(out);

        if (n + strlen(ent.name) + 2 >= cap) {
            break;
        }
        strcat(out, ent.name);
        strcat(out, "|");
    }
}

static void well_formed_rows(const char *opt, const char *what_form)
{
    int err;
    void *n;
    char names[4096], what[200];
    unsigned char want[8192];
    long i;
    struct vfs_stat st;
    struct vfs_statfs sf;

    load(opt);
    {
        int rc = mount_image(VFS_MS_RDONLY, "");

        snprintf(what, sizeof(what), "the well-formed volume mounts (%s): rc %d", what_form, rc);
        check(rc == 0, what);
        if (rc != 0) {
            return;
        }
    }
    listing(mnt.root, names, sizeof(names));
    check(strstr(names, "hello.txt|") && strstr(names, "chained.bin|") &&
              strstr(names, "valid.bin|") && strstr(names, "empty|") &&
              strstr(names, "vendor.txt|") && strstr(names, "straddle.txt|") &&
              strstr(names, "dir|") && strstr(names, "crossing.txt|") &&
              strstr(names, "A long name of some fifty characters for four entries.txt|") &&
              !strstr(names, "FIXTURE"),
          "readdir lists every file of the root and no system entry");
    check_bytes("/hello.txt", (const unsigned char *)"hello exfat\n", 12,
                "hello.txt reads its twelve bytes");
    for (i = 0; i < 4096; i++) {
        want[i] = (unsigned char)(0x10 + i);
        want[4096 + i] = (unsigned char)(0x20 + i);
    }
    n = walk("/chained.bin", &err);
    check(n && read_all(n, back, sizeof(back)) == 12288 && memcmp(back, want, 8192) == 0 &&
              back[8192] == 0x30 && back[12287] == (unsigned char)(0x30 + 4095),
          "chained.bin reads its three clusters through the FAT, across the gaps");
    if (n) {
        OPS->put(&mnt, n);
    }
    for (i = 0; i < 5000; i++) {
        want[i] = (unsigned char)(0x40 + i);
    }
    memset(want + 5000, 0, 8192 - 5000);
    check_bytes("/valid.bin", want, 8192,
                "valid.bin reads its 5000 valid bytes, across its two clusters, and zeros past "
                "them, not the junk");
    n = walk("/empty", &err);
    check(n && OPS->getattr(&mnt, n, &st) == 0 && st.size == 0 &&
              read_all(n, back, sizeof(back)) == 0,
          "the empty file has no bytes");
    if (n) {
        OPS->put(&mnt, n);
    }
    check_bytes("/A long name of some fifty characters for four entries.txt",
                (const unsigned char *)"x", 1, "the name of four entries is found and read");
    check_bytes("/a LONG NAME OF SOME FIFTY characters for four ENTRIES.txt",
                (const unsigned char *)"x", 1, "and by another case, through the up-case table");
    check_bytes("/caf\xc3\xa9.txt", (const unsigned char *)"cafe\n", 5,
                "a name outside ASCII reads");
    check_bytes("/CAF\xc3\x89.TXT", (const unsigned char *)"cafe\n", 5,
                "and by its upper case, which only the table knows, outside ASCII");
    check_bytes("/\xe1\xb8\x81.txt", (const unsigned char *)"ring\n", 5,
                "a name past the table's first run reads");
    check_bytes("/\xe1\xb8\x80.txt", (const unsigned char *)"ring\n", 5,
                "and by its upper case, whose mapping a run has moved from its own index");
    check_bytes("/vendor.txt", (const unsigned char *)"vendor\n", 7,
                "the set with vendor entries reads");
    check_bytes("/straddle.txt", (const unsigned char *)"straddle\n", 9,
                "the set whose file and stream entries straddle a sector reads");
    check_bytes("/crossing.txt", (const unsigned char *)"crossing\n", 9,
                "the set crossing a cluster boundary reads");
    check_bytes("/dir/sub/leaf.txt", (const unsigned char *)"leaf\n", 5,
                "dir/sub/leaf.txt through two levels");
    n = walk("/dir/sub/..", &err);
    check(n && OPS->getattr(&mnt, n, &st) == 0 && (st.mode & 0170000) == 0040000,
          "sub's .. is dir");
    if (n) {
        void *up = 0;

        check(OPS->lookup(&mnt, n, "..", &up) == 0 && up == mnt.root, "and dir's .. is the root");
        if (up) {
            OPS->put(&mnt, up);
        }
        OPS->put(&mnt, n);
    }
    n = walk("/hello.txt", &err);
    check(n && OPS->getattr(&mnt, n, &st) == 0 && st.mtime_sec == 1788264000 &&
              st.mtime_nsec == 0 && st.ctime_sec == 1788264000 && st.size == 12 &&
              (st.mode & 0777) == 0755 && st.uid == 0,
          "the timestamps decode to 2026-09-01 12:00:00 UTC, the mode to 0755");
    if (n) {
        OPS->put(&mnt, n);
    }
    check(walk("/nosuch.txt", &err) == 0 && err == -ENOENT, "a missing name is ENOENT");
    check(OPS->statfs(&mnt, &sf) == 0 && sf.bsize == 4096 && sf.blocks == 4064 && sf.bfree > 4000 &&
              sf.bfree < 4064 && sf.magic == 0x2011BAB0u,
          "statfs reports the cluster geometry and the bitmap's free count");
    unmount_image();
}

static void refused_row(const char *opt, const char *what)
{
    load(opt);
    check(mount_image(VFS_MS_RDONLY, "") == -EINVAL, what);
    unmount_image();
}

static void readonly_row(const char *opt, const char *what)
{
    load(opt);
    check(mount_image(0, "") == 0 && (mnt.flags & VFS_MS_RDONLY), what);
    check_bytes("/hello.txt", (const unsigned char *)"hello exfat\n", 12, "and still reads");
    unmount_image();
}

static void skipped_row(const char *opt, const char *path, const char *what)
{
    int err;
    char names[4096];

    load(opt);
    check(mount_image(VFS_MS_RDONLY, "") == 0, "the volume with a bad set mounts");
    listing(mnt.root, names, sizeof(names));
    check(walk(path, &err) == 0 && err == -ENOENT && !strstr(names, path + 1), what);
    check_bytes("/straddle.txt", (const unsigned char *)"straddle\n", 9,
                "and the sets after it read");
    unmount_image();
}

static void damaged_rows(void)
{
    int err;
    void *n;
    struct vfs_stat st;

    refused_row("--short-fat", "a FAT too short for the clusters is refused");
    refused_row("--fat-in-boot", "a FAT offset inside the boot regions is refused");
    refused_row("--short-bitmap", "a bitmap shorter than the clusters need is refused");
    refused_row("--bad-upcase-checksum", "an up-case table failing its checksum is refused");
    refused_row("--truncated-upcase-run", "a truncated up-case run is refused");
    refused_row("--sector-4k", "a 4 KiB logical sector is refused by name");
    readonly_row("--bad-main-checksum",
                 "a bad main boot checksum with a good backup mounts read-only");
    readonly_row(
        "--main-bad-root",
        "a main region that fails and whose root differs is read from the backup, read-only");
    readonly_row("--bad-backup-checksum", "a bad backup checksum mounts read-only");
    readonly_row("--regions-disagree", "two regions that disagree mount read-only");
    readonly_row("--media-failure", "MediaFailure mounts read-only");
    readonly_row("--second-bitmap", "a second bitmap entry mounts read-only");
    skipped_row("--bad-set-checksum", "/hello.txt", "a set failing its checksum is skipped");
    skipped_row("--wrong-secondary-count", "/hello.txt", "a wrong SecondaryCount is skipped");
    skipped_row("--wrong-name-length", "/hello.txt", "a wrong NameLength is skipped");
    skipped_row("--false-hash", "/hello.txt", "a false name hash is skipped");
    skipped_row("--vdl-past-dl", "/valid.bin", "ValidDataLength past DataLength is skipped");
    skipped_row("--alloc-impossible", "/hello.txt",
                "a stream without AllocationPossible is skipped");

    load("--chain-out");
    check(mount_image(VFS_MS_RDONLY, "") == 0, "a chain leaving the heap mounts");
    n = walk("/chained.bin", &err);
    check(n && read_then_fail(n, 4096), "and its file reads its first cluster, then EIO");
    if (n) {
        OPS->put(&mnt, n);
    }
    unmount_image();
    load("--loop");
    check(mount_image(VFS_MS_RDONLY, "") == 0, "a looped chain mounts");
    n = walk("/chained.bin", &err);
    check(n && read_all(n, back, sizeof(back)) == 12288,
          "and its file reads what it has before the loop");
    if (n) {
        OPS->put(&mnt, n);
    }
    unmount_image();
    load("--short-stream");
    check(mount_image(VFS_MS_RDONLY, "") == 0, "a stream shorter than its length mounts");
    n = walk("/chained.bin", &err);
    check(n && OPS->getattr(&mnt, n, &st) == 0 && st.size == 16384 && read_then_fail(n, 12288),
          "and its file reads its three clusters, then EIO where the chain ends");
    if (n) {
        OPS->put(&mnt, n);
    }
    unmount_image();
    load("--unknown-critical");
    check(mount_image(VFS_MS_RDONLY, "") == 0, "an unknown critical entry in the root mounts");
    check_bytes("/hello.txt", (const unsigned char *)"hello exfat\n", 12,
                "and the sets after it read");
    unmount_image();
    load("--no-utc");
    check(mount_image(VFS_MS_RDONLY, "") == 0, "timestamps without a valid offset mount");
    n = walk("/hello.txt", &err);
    check(n && OPS->getattr(&mnt, n, &st) == 0 && st.mtime_sec == 1788264000,
          "and read as if they were UTC");
    if (n) {
        OPS->put(&mnt, n);
    }
    unmount_image();
    load("--dirty");
    check(mount_image(VFS_MS_RDONLY, "") == 0, "the dirty bit over a clean tree mounts read-only");
    unmount_image();
    load("--zoned");
    check(mount_image(VFS_MS_RDONLY, "") == 0, "timestamps with a zone mount");
    n = walk("/hello.txt", &err);
    check(n && OPS->getattr(&mnt, n, &st) == 0 && st.mtime_sec == 1788264000 - 6 * 3600 &&
              st.mtime_nsec == 340000000 && st.ctime_nsec == 340000000,
          "and a field of 12:00 at +6 hours with 34 hundredths is 06:00:00.34 UTC");
    if (n) {
        OPS->put(&mnt, n);
    }
    unmount_image();
    load("");
    check(mount_image(VFS_MS_RDONLY, "umask=077,uid=7,gid=9") == 0, "the mount options are taken");
    n = walk("/hello.txt", &err);
    check(n && OPS->getattr(&mnt, n, &st) == 0 && (st.mode & 0777) == 0700 && st.uid == 7 &&
              st.gid == 9,
          "and shape the mode and the ids");
    if (n) {
        OPS->put(&mnt, n);
    }
    unmount_image();
    check(mount_image(VFS_MS_RDONLY, "nosuch=1") == -EINVAL, "an unknown option is EINVAL");
}

/* The gate: the volume the host's own tools wrote lists its manifest with
   the right bytes and times. Skipped, and said so, where the host has no
   such tools. */
static void external_rows(void)
{
    char cmd[512], line[600];
    void *m;
    int rc;

    snprintf(cmd, sizeof(cmd), "sh tests/host/mkexfat_external.sh %s", FIXDIR);
    rc = system(cmd);
    rc = rc == -1 ? -1 : (rc >> 8) & 0xff;
    if (rc == 3) {
        printf("exfat_hosttest: the gate is skipped: no newfs_exfat and hdiutil on this host\n");
        return;
    }
    check(rc == 0, "the external volume is made by the host's tools");
    if (rc != 0) {
        return;
    }
    check(load_file(FIXDIR "/exfat_external.img") == 0, "the external volume loads");
    check(mount_image(VFS_MS_RDONLY, "") == 0,
          "the volume newfs_exfat made and the host filled mounts");
    m = fopen(FIXDIR "/exfat_external.manifest", "r");
    check(m != 0, "the manifest exists");
    while (m && fgets(line, sizeof(line), m)) {
        char *bar1 = strchr(line, '|'), *bar2;
        char path[400], what[600], src[600];
        long size, mtime;
        int err;
        void *n;
        struct vfs_stat st;
        void *f;
        static unsigned char want[IMG_BYTES / 64];
        unsigned long got;

        if (!bar1) {
            continue;
        }
        *bar1 = '\0';
        bar2 = strchr(bar1 + 1, '|');
        if (!bar2) {
            continue;
        }
        *bar2 = '\0';
        size = atol(bar1 + 1);
        mtime = atol(bar2 + 1);
        snprintf(path, sizeof(path), "/%s", line);
        snprintf(what, sizeof(what), "the gate: %s is found", line);
        n = walk(path, &err);
        check(n != 0, what);
        if (!n) {
            continue;
        }
        snprintf(what, sizeof(what),
                 "the gate: %s has %ld bytes and its modification time within two seconds", line,
                 size);
        check(OPS->getattr(&mnt, n, &st) == 0 && (long)st.size == size && st.mtime_sec <= mtime &&
                  st.mtime_sec + 2 > mtime,
              what);
        snprintf(src, sizeof(src), "%s/exfat_src/%s", FIXDIR, line);
        f = fopen(src, "rb");
        got = f ? fread(want, 1, sizeof(want), f) : 0;
        if (f) {
            fclose(f);
        }
        snprintf(what, sizeof(what), "the gate: %s reads the bytes the host wrote", line);
        check(f && (long)got == size && read_all(n, back, sizeof(back)) == size &&
                  memcmp(back, want, (unsigned long)size) == 0,
              what);
        OPS->put(&mnt, n);
    }
    if (m) {
        fclose(m);
    }
    unmount_image();
}

int main(void)
{
    proc_a.pid = 1;
    well_formed_rows("", "the held up-case table");
    well_formed_rows("--big-upcase", "the streamed up-case table");
    well_formed_rows("--uncompressed-upcase", "the indexed up-case table");
    damaged_rows();
    external_rows();
    if (fails == 0) {
        printf("exfat_hosttest: OK\n");
        return 0;
    }
    printf("exfat_hosttest: %d failure(s)\n", fails);
    return 1;
}
