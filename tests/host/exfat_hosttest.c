/*
 * exfat_hosttest.c: the exfat driver over the buffer cache over an image.
 *
 * host-deps: rach2/sys/fs/exfat.c rach2/sys/fs/bcache.c rach2/sys/block/chain.c
 * host-deps: rach2/sys/fs/fatcommon.c
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-deps: tests/host/compat/kheap_host.c tests/host/compat/devmodel.c
 * host-deps: tests/host/compat/pressure_host.c
 * host-seconds: 150
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
 * file it damages. The last row: a volume the host's own tools formatted
 * and filled mounts and lists every file in its manifest with the right bytes
 * and times.
 */

#include "../../rach2/sys/fs/exfat.h"
#include "../../rach2/sys/fs/bcache.h"
#include "../../rach2/sys/lib/kerrno.h"
#include "../../rach2/sys/proc/process.h"
#include "devmodel.h"

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
extern int strncmp(const char *a, const char *b, unsigned long n);
extern unsigned long fwrite(const void *buf, unsigned long size, unsigned long n, void *f);
extern void *popen(const char *cmd, const char *mode);
extern int pclose(void *f);

#define FIXDIR    "/tmp/rach2_verify"
#define IMG_BYTES (65536u * 512u) /* 32 MiB: the fixture is 16, the external volume 16 */

/* ---- what the driver needs of the kernel ---- */

static process_t proc_a;
process_t *current_proc = &proc_a;

/* The kernel's records go to the log the runner keeps, so a refusal or a
   skipped set names its reason there. */
extern int vsnprintf(char *s, unsigned long n, const char *fmt, __builtin_va_list ap);
static char last_klog[400];
void klog(int level, const char *subsystem, const char *fmt, ...)
{
    __builtin_va_list ap;

    (void)level;
    __builtin_va_start(ap, fmt);
    vsnprintf(last_klog, sizeof(last_klog), fmt, ap);
    __builtin_va_end(ap);
    printf("klog: %s: %s\n", subsystem, last_klog);
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

/* `image` is what the device answers, its medium with the writes pending
   since the last barrier over it; `medium` is what a crash keeps, through
   the model in tests/host/compat/devmodel.c. */
static unsigned char image[IMG_BYTES];
static unsigned char medium[IMG_BYTES];
static unsigned char snap[IMG_BYTES];
static unsigned char snaps[DEVMODEL_KEEP_MOST + 2][IMG_BYTES]; /* the states at one stop */
static struct devmodel model;
static uint64_t image_sectors;

static int img_read(block_device_t *d, uint64_t lba, uint32_t count, void *buf)
{
    (void)d;
    if (lba + count > image_sectors) {
        return -5;
    }
    return devmodel_read(&model, lba, count, buf);
}

static int writes_made;
static int stop_after = -1;

/* Writes go to the model, pending until the driver's next barrier.
   `stop_after` is the interruption matrix's crash: the writes after that
   many fail, and what the crash leaves is the model's snapshot. */
static int img_write(block_device_t *d, uint64_t lba, uint32_t count, const void *buf)
{
    (void)d;
    if (lba + count > image_sectors) {
        return -5;
    }
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

/* The medium as it stands, with nothing pending: what the rows that judge
   the image directly see, since a barrier came before them. */
static void settle(void)
{
    if (model.npending) {
        devmodel_barrier(&model);
    }
}

static block_device_t dev = {512, 32768u, BLOCK_FAULT_SAFE, 0, 0, 0, img_read, img_write, 0, 0, 0};

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
    stop_after = -1;
    model.stopped = 0; /* a stop is over: the cache drains into the old image */
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
    devmodel_load(&model);
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

/* ---- the write rows ---- */

static unsigned char pattern[8192];
static char check_out[16384];
static void fill_pattern(void)
{
    unsigned i;

    for (i = 0; i < sizeof(pattern); i++) {
        pattern[i] = (unsigned char)(i * 7 + 3);
    }
}

/* Mount the loaded image writable; the constructor's rc. */
static int mount_rw(void)
{
    __builtin_memset(&mnt, 0, sizeof(mnt));
    mnt.type = &vfs_exfat_type;
    mnt.dev = &dev;
    mnt.flags = 0;
    mnt.ns_holder = -1;
    if (vfs_exfat_type.mount(&mnt, &dev, 0, "") != 0) {
        return -1;
    }
    vfs_ns_lock(&mnt); /* the test stands in for the syscall layer */
    return 0;
}

static void *make_file(void *dir, const char *name)
{
    void *n = 0;

    if (OPS->create(&mnt, dir, name, 0644, &n) != 0) {
        return 0;
    }
    return n;
}

static int write_at(void *node, const unsigned char *buf, uint32_t count, uint64_t off)
{
    void *h;
    int32_t got;

    if (OPS->open(&mnt, node, 0, &h) != 0) {
        return -1;
    }
    got = OPS->write(&mnt, h, buf, count, off);
    OPS->release(&mnt, h);
    return got == (int32_t)count ? 0 : (got < 0 ? got : -1);
}

/* Dump the image and walk it with the host reader; the check's exit
   status, its output in `out`. */
static int run_check_on(const unsigned char *what, char *out, unsigned long cap)
{
    char cmd[300], line[300];
    void *f;
    unsigned long n = 0;
    int rc;

    f = fopen(FIXDIR "/exfat_write.img", "wb");
    if (!f || fwrite(what, 1, (unsigned long)image_sectors * 512, f) !=
                  (unsigned long)image_sectors * 512) {
        printf("exfat_hosttest: cannot dump the image\n");
        exit(1);
    }
    fclose(f);
    snprintf(cmd, sizeof(cmd), "%s/mkexfatfixture --check %s/exfat_write.img", FIXDIR, FIXDIR);
    f = popen(cmd, "r");
    if (!f) {
        printf("exfat_hosttest: cannot run the check\n");
        exit(1);
    }
    out[0] = '\0';
    while (fgets(line, sizeof(line), f) && n + strlen(line) < cap - 1) {
        memcpy(out + n, line, strlen(line) + 1);
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
        printf("exfat_hosttest: the cache did not empty for a snapshot\n");
        exit(1);
    }
    __builtin_memcpy(image, what, (unsigned long)image_sectors * 512);
    devmodel_load(&model);
}

/* The image kept under a name, for the external checker run by hand. */
static void dump_named(const char *kind, const char *name)
{
    char path[200];
    void *f;

    snprintf(path, sizeof(path), "%s/exfat_%s_%s.img", FIXDIR, kind, name);
    f = fopen(path, "wb");
    if (f) {
        fwrite(image, 1, (unsigned long)image_sectors * 512, f);
        fclose(f);
    }
}

/* Read the crash state back from the dump: the cache's dirty buffers go
   into the image the dump replaces, so nothing of them survives. */
static void load_dump(void)
{
    stop_after = -1;
    model.stopped = 0;
    bcache_sync(&dev);
    if (load_file(FIXDIR "/exfat_write.img") != 0) {
        printf("exfat_hosttest: cannot read the dump back\n");
        exit(1);
    }
}

/* The findings the driver's own order may leave behind a stop, and no
   other: lost clusters, orphan secondaries, a checksum-only set, a
   surplus tail; the three the check repairs, and the one it counts. */
static int findings_allowed(const char *out)
{
    static const char *const allowed[] = {"check: OK: ",
                                          "check: FAIL: ",
                                          "check: lost: ",
                                          "check: orphan secondaries: ",
                                          "check: checksum-only: ",
                                          "check: surplus: "};
    const char *p = out;

    while (*p) {
        const char *nl = strchr(p, '\n');
        int ok = strncmp(p, "check: ", 7) != 0, a;

        for (a = 0; a < 6 && !ok; a++) {
            if (strncmp(p, allowed[a], strlen(allowed[a])) == 0) {
                ok = 1;
            }
        }
        if (!ok) {
            return 0;
        }
        if (!nl) {
            break;
        }
        p = nl + 1;
    }
    return 1;
}

/* ---- the sequences ---- */

static int seq_create(void)
{
    void *n = make_file(mnt.root, "w one.txt");

    if (!n) {
        return -1;
    }
    if (write_at(n, (const unsigned char *)"hello", 5, 0) != 0) {
        OPS->put(&mnt, n);
        return -1;
    }
    OPS->put(&mnt, n);
    return 0;
}

static int seq_extend(void)
{
    void *n = make_file(mnt.root, "w two.bin");
    int rc;

    if (!n) {
        return -1;
    }
    rc = write_at(n, pattern, 2010, 0);
    if (rc == 0) {
        rc = write_at(n, pattern + 2010, 3000, 2010);
    }
    OPS->put(&mnt, n);
    return rc;
}

static int seq_gap(void)
{
    void *n = make_file(mnt.root, "w three.bin");
    int rc;

    if (!n) {
        return -1;
    }
    rc = write_at(n, pattern, 10, 0);
    if (rc == 0) {
        rc = write_at(n, pattern + 10, 5, 6000); /* zeros between, across a cluster */
    }
    OPS->put(&mnt, n);
    return rc;
}

static int seq_truncate(void)
{
    void *n = make_file(mnt.root, "w four.txt");
    void *h;
    int rc;

    if (!n) {
        return -1;
    }
    rc = write_at(n, pattern, 6010, 0);
    if (rc == 0 && OPS->open(&mnt, n, 0, &h) == 0) {
        rc = OPS->truncate(&mnt, h, 700);
        if (rc == 0) {
            rc = OPS->truncate(&mnt, h, 1000); /* a grow inside the last cluster */
        }
        if (rc == 0) {
            rc = OPS->truncate(&mnt, h, 6600); /* and one across a cluster */
        }
        if (rc == 0) {
            rc = OPS->truncate(&mnt, h, 0);
        }
        if (rc == 0) {
            rc = OPS->truncate(&mnt, h, 5000);
        }
        OPS->release(&mnt, h);
    }
    OPS->put(&mnt, n);
    return rc;
}

static int seq_unlink(void)
{
    void *n = make_file(mnt.root, "w five.txt");
    void *h;
    int rc;

    if (!n) {
        return -1;
    }
    rc = write_at(n, pattern, 100, 0);
    if (rc == 0 && OPS->open(&mnt, n, 0, &h) == 0) {
        rc = OPS->unlink(&mnt, mnt.root, "w five.txt");
        if (rc == 0) {
            rc = write_at(n, pattern, 4200, 0); /* through the description, held */
        }
        if (rc == 0) {
            /* Another file written meanwhile must not take the held
               file's clusters: its bytes are still its own afterwards. */
            void *o = make_file(mnt.root, "w six.txt");

            rc = o ? write_at(o, pattern + 100, 4200, 0) : -1;
            if (o) {
                OPS->put(&mnt, o);
            }
            if (rc == 0 &&
                (read_all(n, back, sizeof(back)) != 4200 || memcmp(back, pattern, 4200) != 0)) {
                rc = -1;
            }
        }
        OPS->release(&mnt, h);
    }
    OPS->put(&mnt, n);
    return rc;
}

static int seq_mkdir(void)
{
    void *d = 0, *n;
    int rc = OPS->mkdir(&mnt, mnt.root, "Dir One", 0755);

    if (rc != 0 || OPS->lookup(&mnt, mnt.root, "Dir One", &d) != 0) {
        return -1;
    }
    n = make_file(d, "inner.txt");
    rc = n ? write_at(n, pattern, 30, 0) : -1;
    if (n) {
        OPS->put(&mnt, n);
    }
    if (rc == 0 && OPS->rmdir(&mnt, mnt.root, "Dir One") != -ENOTEMPTY) {
        rc = -1;
    }
    if (rc == 0) {
        rc = OPS->unlink(&mnt, d, "inner.txt");
    }
    OPS->put(&mnt, d);
    if (rc == 0) {
        rc = OPS->rmdir(&mnt, mnt.root, "Dir One");
    }
    return rc;
}

static int seq_rename(void)
{
    void *n = make_file(mnt.root, "r1.txt");
    void *d = 0, *o;
    int rc;

    if (!n) {
        return -1;
    }
    rc = write_at(n, pattern, 40, 0);
    OPS->put(&mnt, n);
    if (rc == 0) {
        rc = OPS->rename(&mnt, mnt.root, "r1.txt", mnt.root, "R1x.txt"); /* in place */
    }
    if (rc == 0 && OPS->lookup(&mnt, mnt.root, "dir", &d) != 0) {
        rc = -1;
    }
    if (rc == 0) {
        rc = OPS->rename(&mnt, mnt.root, "R1x.txt", d, "moved name.txt"); /* across */
    }
    if (rc == 0) {
        o = make_file(mnt.root, "over.txt");
        rc = o ? write_at(o, pattern + 100, 20, 0) : -1;
        if (o) {
            OPS->put(&mnt, o);
        }
    }
    if (rc == 0) {
        rc = OPS->rename(&mnt, d, "moved name.txt", mnt.root, "over.txt"); /* over a file */
    }
    if (d) {
        OPS->put(&mnt, d);
    }
    return rc;
}

/* A write past the valid length of the fixture's valid.bin, whose bytes
   past it are junk on the disk: the gap is zeroed before the length is
   published. */
static int seq_vdl(void)
{
    void *n;
    int rc;

    if (OPS->lookup(&mnt, mnt.root, "valid.bin", &n) != 0) {
        return -1;
    }
    rc = write_at(n, pattern, 10, 7000);
    OPS->put(&mnt, n);
    return rc;
}

/* The fixture's contiguous hello.txt grows: its run into the FAT, the
   flag cleared, then the chain joined to. */
static int seq_nofat(void)
{
    void *n;
    int rc;

    if (OPS->lookup(&mnt, mnt.root, "hello.txt", &n) != 0) {
        return -1;
    }
    rc = write_at(n, pattern, 5000, 12);
    OPS->put(&mnt, n);
    return rc;
}

/* The fixture's straddle.txt, whose file and stream entries lie in two
   sectors: a change is two writes, the file entry's last. */
static int seq_straddle(void)
{
    void *n;
    struct vfs_stat st;
    int rc;

    if (OPS->lookup(&mnt, mnt.root, "straddle.txt", &n) != 0) {
        return -1;
    }
    st.mode = 0555;
    rc = OPS->setattr(&mnt, n, VFS_SET_MODE, &st);
    if (rc == 0) {
        rc = write_at(n, pattern, 100, 9); /* the lengths change too */
    }
    OPS->put(&mnt, n);
    return rc;
}

static const struct {
    const char *name;
    int (*run)(void);
} sequences[] = {
    {"create", seq_create},     {"extend", seq_extend}, {"gap", seq_gap},
    {"truncate", seq_truncate}, {"unlink", seq_unlink}, {"mkdir", seq_mkdir},
    {"rename", seq_rename},     {"nofat", seq_nofat},   {"straddle", seq_straddle},
    {"vdl", seq_vdl},
};

/* Files made and remade: what a program rewriting a set of files does, the
   set found, truncated and written again, many times over. Not in the
   matrix, which would stop after each of its hundreds of writes. */
static int rewrite_rows_run(void)
{
    unsigned i, round;

    for (round = 0; round < 2; round++) {
        for (i = 0; i < 200; i++) {
            char name[16];
            void *n, *h;
            int rc;

            snprintf(name, sizeof(name), "f%03u", i);
            if (OPS->lookup(&mnt, mnt.root, name, &n) != 0) {
                int crc = OPS->create(&mnt, mnt.root, name, 0644, &n);

                if (crc != 0) {
                    printf("exfat_hosttest: rewrite: create %s failed in round %u (%d)\n", name,
                           round, crc);
                    return -1;
                }
            }
            if (OPS->open(&mnt, n, 0, &h) != 0) {
                OPS->put(&mnt, n);
                return -1;
            }
            rc = OPS->truncate(&mnt, h, 0);
            if (rc == 0) {
                rc = OPS->write(&mnt, h, pattern, 1024, 0) == 1024 ? 0 : -1;
            }
            OPS->release(&mnt, h);
            OPS->put(&mnt, n);
            if (rc != 0) {
                printf("exfat_hosttest: rewrite: %s failed in round %u (%d)\n", name, round, rc);
                return -1;
            }
        }
    }
    return 0;
}

/* The content oracle: after a stop, every file a sequence wrote reads,
   within its published length, the bytes written to it or zeros, and the
   fixture's own files are intact and reachable. */
static int oracle_ok(void)
{
    static const char *const names[] = {"w one.txt", "w two.bin", "w three.bin", "w four.txt",
                                        "over.txt",  "R1x.txt",   "r1.txt",      "w six.txt",
                                        "valid.bin", "hello.txt", "straddle.txt"};
    static const char *const fixed[] = {"chained.bin", "dir/sub/leaf.txt", "crossing.txt"};
    int f, err;
    long i;

    for (f = 0; f < 11; f++) {
        void *n;
        long got;
        int zeros = 1, written = 1;
        char path[64];

        snprintf(path, sizeof(path), "/%s", names[f]);
        n = walk(path, &err);
        if (!n) {
            continue;
        }
        got = read_all(n, back, sizeof(back));
        OPS->put(&mnt, n);
        if (got < 0) {
            printf("exfat_hosttest: %s does not read (%ld)\n", names[f], got);
            return 0;
        }
        for (i = 0; i < got; i++) {
            unsigned char want;

            if (f == 0) {
                want = i < 5 ? (unsigned char)"hello"[i] : 0;
            } else if (f == 2) {
                want = i < 10 ? pattern[i] : (i >= 6000 ? pattern[10 + i - 6000] : 0);
            } else if (f == 3 && got == 5000) {
                want = 0; /* regrown from nothing after the truncate to zero */
            } else if (f == 3 && (got == 1000 || got == 6600)) {
                want = i < 700 ? pattern[i] : 0; /* grown from 700 by truncate */
            } else if (f == 4 && got == 20) {
                want = pattern[100 + i]; /* over.txt before the rename over it */
            } else if (f == 7) {
                want = pattern[100 + i];
            } else if (f == 8) {
                /* valid.bin: the fixture's bytes, zeros past 5000, and at
                   7000 the ten bytes the vdl sequence writes, or zeros
                   before it has. */
                want = i < 5000 ? (unsigned char)(0x40 + i) : 0;
                if (i >= 7000 && i < 7010 && back[i] == pattern[i - 7000]) {
                    want = back[i];
                }
            } else if (f == 9) {
                /* hello.txt: its twelve bytes, then what the nofat sequence
                   writes past them, or zeros before it has. */
                want = i < 12 ? (unsigned char)"hello exfat\n"[i] : pattern[i - 12];
                if (i >= 12 && back[i] == 0) {
                    want = 0;
                }
            } else if (f == 10) {
                /* straddle.txt: its nine bytes, then the hundred the
                   straddle sequence writes at 9, or zeros before it has. */
                want = i < 9 ? (unsigned char)"straddle\n"[i] : pattern[i - 9];
                if (i >= 9 && back[i] == 0) {
                    want = 0;
                }
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
            printf("exfat_hosttest: %s holds bytes nobody wrote (%ld bytes)\n", names[f], got);
            return 0;
        }
    }
    for (f = 0; f < 3; f++) {
        char path[64];
        void *n;

        snprintf(path, sizeof(path), "/%s", fixed[f]);
        n = walk(path, &err);
        if (!n || read_all(n, back, sizeof(back)) < 0) {
            printf("exfat_hosttest: the fixture's %s is gone or unreadable after the stop\n",
                   fixed[f]);
            if (n) {
                OPS->put(&mnt, n);
            }
            return 0;
        }
        OPS->put(&mnt, n);
    }
    return 1;
}

/* The volume walked clean, or with the findings a stop may leave. */
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

/* One stop's states, judged: the walk of each and the content oracle,
   with none, the newest one, two, and so on, or all of the writes pending
   since the driver's last barrier kept, each a state a drive that keeps
   writes pending can leave. */
static void judge_stop(unsigned i, const char *stage)
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
        if (mount_image(VFS_MS_RDONLY, "") == 0) {
            snprintf(what, sizeof(what),
                     "'%s' %s, %s of the pending writes kept, reads only what was written",
                     sequences[i].name, stage, name);
            check(oracle_ok(), what);
            unmount_image();
        } else {
            snprintf(what, sizeof(what), "'%s' %s, %s of the pending writes kept, mounts",
                     sequences[i].name, stage, name);
            check(0, what);
        }
        snprintf(what, sizeof(what),
                 "'%s' %s, %s of the pending writes kept, leaves a consistent volume",
                 sequences[i].name, stage, name);
        check(findings_allowed(check_out), what);
        if (!findings_allowed(check_out)) {
            printf("exfat_hosttest: the check said:\n%s", check_out);
        }
    }
}

static void check_consistent(const char *what, int allow_stop_states)
{
    int rc = run_check(check_out, sizeof(check_out));

    if (allow_stop_states ? !findings_allowed(check_out) : rc != 0) {
        check(0, what);
        printf("exfat_hosttest: the check said:\n%s", check_out);
    }
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
    if (mount_rw() != 0) {
        check(0, "the durability row's volume mounts");
        return;
    }
    n = make_file(mnt.root, "durable.bin");
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
    unmount_image();
    devmodel_snapshot(&model, DEVMODEL_KEEP_NONE, snap);
    load_snapshot(snap);
    if (mount_image(VFS_MS_RDONLY, "") != 0) {
        check(0, "the medium after the sync mounts");
        return;
    }
    n = walk("/durable.bin", &err);
    check(n != 0 && read_all(n, back, sizeof(back)) == 3000 && memcmp(back, pattern, 3000) == 0,
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

static void write_rows(void)
{
    unsigned i;
    int err;
    void *n;
    struct vfs_stat st;

    fill_pattern();
    for (i = 0; i < sizeof(sequences) / sizeof(sequences[0]); i++) {
        char what[120];

        load("--junk");
        check(mount_rw() == 0, "a writable mount for the write rows");
        check(image[106] & 2, "the dirty bit is on the bytes from the mount");
        snprintf(what, sizeof(what), "the '%s' sequence runs", sequences[i].name);
        check(sequences[i].run() == 0, what);
        check(oracle_ok(), "and its files read what was written");
        check(OPS->prepare_unmount(&mnt) == 0, "its clean unmount");
        unmount_image();
        check(!(image[106] & 2), "and the dirty bit is off after");
        snprintf(what, sizeof(what), "the volume is consistent after '%s'", sequences[i].name);
        check_consistent(what, 0);
        dump_named("seq", sequences[i].name);
    }
    /* What the sequences left, read through the reader mounted fresh. */
    load("--junk");
    check(mount_rw() == 0, "a writable mount for the read-back");
    check(seq_extend() == 0 && seq_rename() == 0, "extend and rename again");
    n = walk("/w two.bin", &err);
    check(n && OPS->getattr(&mnt, n, &st) == 0 && st.size == 5010 &&
              read_all(n, back, sizeof(back)) == 5010 && memcmp(back, pattern, 5010) == 0,
          "the extended file reads its 5010 bytes across the cluster boundary");
    if (n) {
        OPS->put(&mnt, n);
    }
    check(walk("/R1x.txt", &err) == 0 && walk("/dir/moved name.txt", &err) == 0,
          "the renamed names are gone");
    n = walk("/over.txt", &err);
    check(n && read_all(n, back, sizeof(back)) == 40 && memcmp(back, pattern, 40) == 0,
          "and the file renamed over another has the source's bytes");
    if (n) {
        OPS->put(&mnt, n);
    }
    check(OPS->create(&mnt, mnt.root, "hello.txt", 0644, &n) == -EEXIST &&
              OPS->create(&mnt, mnt.root, "HELLO.TXT", 0644, &n) == -EEXIST,
          "a name that up-cases alike is one name: EEXIST");
    check(OPS->prepare_unmount(&mnt) == 0, "the clean unmount");
    unmount_image();
    load("--junk");
    check(mount_rw() == 0, "a writable mount for the rewrite rows");
    check(rewrite_rows_run() == 0, "two hundred files made and remade twice over");
    check(OPS->prepare_unmount(&mnt) == 0, "the rewrite rows' clean unmount");
    unmount_image();
    check_consistent("the volume is consistent after the rewrites", 0);
    /* A vendor allocation's clusters go with its set, and the percent in
       use on the disk is the bitmap's. */
    load("--junk");
    check(mount_rw() == 0, "a writable mount for the vendor and boot-state rows");
    check(OPS->unlink(&mnt, mnt.root, "vendor.txt") == 0,
          "the set with a vendor allocation is unlinked");
    check(OPS->prepare_unmount(&mnt) == 0, "its clean unmount");
    unmount_image();
    check_consistent("and its vendor clusters are not left allocated", 0);

    /* A contiguous file extended on a full volume: ENOSPC, and its stream
       still says NoFatChain, since nothing was written. */
    load("--junk");
    check(mount_rw() == 0, "a writable mount for the ENOSPC rows");
    {
        void *big = make_file(mnt.root, "fill.bin");
        int rc = 0;
        uint64_t at = 0;

        while (big && rc == 0 && at < 32u * 1024 * 1024) {
            rc = write_at(big, pattern, 8192, at);
            at += 8192;
        }
        /* The last clusters, one at a time, so that none is left free. */
        rc = 0;
        while (big && rc == 0) {
            struct vfs_stat st2;

            if (OPS->getattr(&mnt, big, &st2) != 0) {
                break;
            }
            rc = write_at(big, pattern, 4096, st2.size);
        }
        check(big && rc == -ENOSPC, "the volume fills and answers ENOSPC");
        if (big) {
            OPS->put(&mnt, big);
        }
    }
    n = walk("/hello.txt", &err);
    check(n && write_at(n, pattern, 4096, 12) == -ENOSPC,
          "a contiguous file cannot grow on a full volume");
    if (n) {
        OPS->put(&mnt, n);
    }
    check(OPS->prepare_unmount(&mnt) == 0, "the full volume's clean unmount");
    unmount_image();
    {
        char cmd[300], line[300];
        void *pf;
        int nofat_seen = 0;
        uint32_t used = 0, c, heap, spc, clusters;
        unsigned char *bitmap;

        run_check(check_out, sizeof(check_out));
        snprintf(cmd, sizeof(cmd), "%s/mkexfatfixture --check %s/exfat_write.img --list", FIXDIR,
                 FIXDIR);
        pf = popen(cmd, "r");
        while (pf && fgets(line, sizeof(line), pf)) {
            if (strstr(line, "/hello.txt") && strstr(line, "nofat")) {
                nofat_seen = 1;
            }
        }
        if (pf) {
            pclose(pf);
        }
        check(nofat_seen, "and its stream still says NoFatChain: the refusal wrote nothing");
        heap = image[88] | (image[89] << 8) | (image[90] << 16) | ((uint32_t)image[91] << 24);
        spc = image[109];
        clusters = image[92] | (image[93] << 8) | (image[94] << 16) | ((uint32_t)image[95] << 24);
        bitmap = image + ((uint64_t)heap + (0u << spc)) * 512; /* the bitmap is cluster 2 */
        for (c = 0; c < clusters; c++) {
            if (bitmap[c / 8] & (1u << (c % 8))) {
                used++;
            }
        }
        check(image[112] == (unsigned char)(used * 100u / clusters),
              "the percent in use on the disk is the bitmap's, rounded down");
    }
    /* The placement rule: no set this driver makes has its file entry in a
       sector's last slot, so its file and stream entries share a sector.
       The fixture's own straddle.txt is one, on purpose; the rule is read
       over the entries made here, past the fixture's end marker, in the
       root's first two clusters. */
    load("--junk");
    {
        uint32_t root_cluster, heap, spc, fat, second, first_new = 0, at_last = 0, made = 0;
        unsigned char *c1, *c2;

        root_cluster =
            image[96] | (image[97] << 8) | (image[98] << 16) | ((uint32_t)image[99] << 24);
        heap = image[88] | (image[89] << 8) | (image[90] << 16) | ((uint32_t)image[91] << 24);
        fat = image[80] | (image[81] << 8) | (image[82] << 16) | ((uint32_t)image[83] << 24);
        spc = image[109];
        c1 = image + ((uint64_t)heap + ((uint64_t)(root_cluster - 2) << spc)) * 512;
        while (first_new < 256 && (first_new < 128 ? c1[first_new * 32] : 0) != 0x00) {
            first_new++;
        }
        check(mount_rw() == 0, "a writable mount for the placement rows");
        for (i = 0; i < 20; i++) {
            char name[24];

            snprintf(name, sizeof(name), "place %02u.txt", i);
            n = make_file(mnt.root, name);
            if (n) {
                made++;
                OPS->put(&mnt, n);
            }
        }
        check(OPS->prepare_unmount(&mnt) == 0, "the placement rows' unmount");
        unmount_image();
        /* The root's second cluster, through the FAT, once it exists. */
        second = image[fat * 512 + root_cluster * 4] |
                 (image[fat * 512 + root_cluster * 4 + 1] << 8) |
                 (image[fat * 512 + root_cluster * 4 + 2] << 16) |
                 ((uint32_t)image[fat * 512 + root_cluster * 4 + 3] << 24);
        c2 = second >= 2 && second != 0xFFFFFFFFu
                 ? image + ((uint64_t)heap + ((uint64_t)(second - 2) << spc)) * 512
                 : 0;
        for (i = first_new; i < 256; i++) {
            const unsigned char *e = i < 128 ? c1 + i * 32 : (c2 ? c2 + (i - 128) * 32 : 0);

            if (e && e[0] == 0x85 && i % 16 == 15) {
                at_last++;
            }
        }
        check(made == 20 && at_last == 0,
              "twenty files created and no file entry of theirs in a sector's last slot");
    }
    /* The backup boot region is never written: not while the dirty bit is
       on the main sector, and not by the clean unmount. */
    load("--junk");
    memcpy(back, image + 12 * 512, 12 * 512);
    check(mount_rw() == 0 && seq_create() == 0 && memcmp(back, image + 12 * 512, 12 * 512) == 0,
          "a mount and a write leave the backup boot region byte for byte");
    check(OPS->prepare_unmount(&mnt) == 0 && memcmp(back, image + 12 * 512, 12 * 512) == 0,
          "and so does the clean unmount");
    unmount_image();
    /* A frozen directory: the root holding a critical entry this driver
       does not know refuses every change with EROFS. */
    load("--unknown-critical");
    check(mount_rw() == 0, "the volume with an unknown critical entry mounts writable");
    check(OPS->create(&mnt, mnt.root, "x.txt", 0644, &n) == -EROFS &&
              OPS->mkdir(&mnt, mnt.root, "d", 0755) == -EROFS,
          "and its root is frozen: a create or a mkdir in it is EROFS");
    n = walk("/dir", &err);
    check(n && OPS->create(&mnt, n, "x.txt", 0644, &n) == 0,
          "while a directory below it takes a file");
    check(OPS->prepare_unmount(&mnt) == 0, "and the unmount is clean");
    unmount_image();
}

/* Every sequence stopped after each of its writes; the image walked each
   time, allowed only what the order of writes leaves behind, and the
   crash state read for what was written. */
static void matrix_pass(void)
{
    unsigned i;
    int k, total = 0;

    for (i = 0; i < sizeof(sequences) / sizeof(sequences[0]); i++) {
        int n, barriers, b;

        load("--junk");
        stop_after = -1;
        if (mount_rw() != 0) {
            check(0, "the matrix's volume mounts");
            return;
        }
        writes_made = 0; /* the mount's own dirty-bit write is not a stop */
        model.barriers = 0;
        sequences[i].run();
        n = writes_made;
        OPS->put(&mnt, mnt.root);
        OPS->unmount(&mnt);
        barriers = model.barriers;
        /* Every stop, and at each the three states the medium can be in:
           none, only the newest, or all of the writes pending since the
           driver's last barrier. A barrier left out widens what is pending
           at the stops after it, and one of the states then shows the
           write that should have waited. */
        for (k = 1; k <= n; k++) {
            char stage[60];

            load("--junk");
            if (mount_rw() != 0) {
                check(0, "the matrix's volume mounts again");
                stop_after = -1;
                return;
            }
            writes_made = 0;
            stop_after = k;
            sequences[i].run();
            OPS->put(&mnt, mnt.root);
            OPS->unmount(&mnt);
            snprintf(stage, sizeof(stage), "stopped after write %d of %d", k, n);
            if (k == n - 1) {
                devmodel_snapshot(&model, DEVMODEL_PENDING_MAX, snap);
                run_check_on(snap, check_out, sizeof(check_out));
                dump_named("stop",
                           sequences[i].name); /* one stop of each, for the external checker */
            }
            judge_stop(i, stage);
            total++;
        }
        /* Each barrier of the sequence made to fail, the driver's answer
           whatever it is, and the medium judged with half of what was
           pending at it; then the machine stopped at each of the four
           writes after the failure, since what the driver does to undo a
           failed step has an order of its own. */
        for (b = 1; b <= barriers; b++) {
            char what[200];
            int failed_at, j;

            load("--junk");
            if (mount_rw() != 0) {
                check(0, "the matrix's volume mounts for a failing barrier");
                return;
            }
            writes_made = 0;
            model.writes = 0; /* the model's count of the failure is this count */
            stop_after = -1;
            model.barriers = 0;
            model.fail_barrier = b;
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
                printf("exfat_hosttest: the check said:\n%s", check_out);
            }
            total++;
            for (j = 0; j <= 3; j++) {
                load("--junk");
                if (mount_rw() != 0) {
                    check(0, "the matrix's volume mounts for a failing barrier and a stop");
                    stop_after = -1;
                    return;
                }
                writes_made = 0;
                model.writes = 0;
                stop_after = failed_at + j;
                model.barriers = 0;
                model.fail_barrier = b;
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
    printf("exfat_hosttest: %d stops walked\n", total);
}

/* ---- the dirty rows ---- */

static unsigned char image_before[IMG_BYTES];

/* A volume under the dirty bit mounted writable: the check runs, repairs
   or refuses. `want_ro` says which; `what` names the row. */
static void dirty_mount(const char *opt, int want_ro, const char *what)
{
    load(opt);
    memcpy(image_before, image, (unsigned long)image_sectors * 512);
    check(mount_rw() == 0 && ((mnt.flags & VFS_MS_RDONLY) != 0) == want_ro, what);
}

static void dirty_rows(void)
{
    int err;
    void *n;
    struct vfs_statfs sf;
    /* Each fault and the reason the check must name for it. */
    static const char *const faults[][2] = {
        {"--dirty-overlap", "a cluster reached twice"},
        {"--dirty-short-stream", "a stream shorter than its length"},
        {"--dirty-reached-free", "a reached cluster whose bitmap bit is clear"},
        {"--dirty-wrong-name-length", "units past NameLength"},
        {"--dirty-critical", "a critical entry this driver does not know"},
        {"--dirty-dup", "two names that up-case alike in one directory"},
        {"--dirty-loop", "a cluster reached twice"}, /* the loop returns to a marked cluster */
        {"--dirty-chain-out", "a chain outside the heap"},
        {"--dirty-hash-and-checksum", "the name hash"},
        {"--dirty-swallow", "an entry of the set that is not an in-use secondary"},
        {"--dirty-label-below", "a root-only critical entry in a subdirectory"}};
    static const char *const repairs[] = {"--dirty-bad-set-checksum", "--dirty-orphan",
                                          "--dirty-surplus", "--dirty-all"};
    unsigned f;
    int k;

    dirty_mount("--dirty", 0, "a dirty volume with a clean tree mounts writable");
    check(image[106] & 2, "with its bit still set");
    check(OPS->prepare_unmount(&mnt) == 0, "its clean unmount");
    unmount_image();
    check(!(image[106] & 2), "and the bit clears only then");
    check_consistent("the clean tree is clean after", 0);

    dirty_mount("--dirty-bad-set-checksum", 0,
                "a set failing only its checksum is repaired and the volume mounts writable");
    check_bytes("/hello.txt", (const unsigned char *)"hello exfat\n", 12,
                "and the file is reachable");
    check(OPS->prepare_unmount(&mnt) == 0, "its clean unmount");
    unmount_image();
    check_consistent("and the walk finds the checksum right", 0);

    dirty_mount("--dirty-orphan", 0, "orphan secondaries are freed and the volume mounts writable");
    check(OPS->prepare_unmount(&mnt) == 0, "its clean unmount");
    unmount_image();
    check_consistent("and the walk finds no orphan", 0);

    dirty_mount("--dirty-surplus", 0, "a surplus tail is freed and the volume mounts writable");
    n = walk("/chained.bin", &err);
    check(n && read_all(n, back, sizeof(back)) == 12288, "the file the tail hung off reads");
    if (n) {
        OPS->put(&mnt, n);
    }
    check(OPS->statfs(&mnt, &sf) == 0 && sf.bfree == 4064 - 21,
          "and the freed cluster is free in the count the check left");
    check(OPS->prepare_unmount(&mnt) == 0, "its clean unmount");
    unmount_image();
    check_consistent("and the walk finds no surplus and no lost cluster", 0);

    dirty_mount("--dirty-nofat-orphan", 0,
                "orphans in the second cluster of a contiguous directory are freed");
    check(OPS->prepare_unmount(&mnt) == 0, "its clean unmount");
    unmount_image();
    check_consistent("and the walk finds no orphan there", 0);

    dirty_mount("--dirty-dup-checksum", 1,
                "a duplicate name whose earlier set is checksum-only mounts read-only");
    check(strstr(last_klog, "two names that up-case alike") != 0,
          "and names the duplicate, not the checksum");
    unmount_image();

    dirty_mount("--dirty-all", 0, "the three repairable states at once mount writable");
    check_bytes("/hello.txt", (const unsigned char *)"hello exfat\n", 12,
                "the repaired set is reachable");
    n = walk("/chained.bin", &err);
    check(n && read_all(n, back, sizeof(back)) == 12288, "the file the tail hung off reads");
    if (n) {
        OPS->put(&mnt, n);
    }
    check(OPS->prepare_unmount(&mnt) == 0, "its clean unmount");
    unmount_image();
    check_consistent("and the walk finds none of the three", 0);

    dirty_mount("--dirty-lost", 0, "a lost chain leaves the volume writable");
    check(OPS->statfs(&mnt, &sf) == 0 && sf.bfree == 4064 - 23, /* 21 of the fixture, 2 lost */
          "with the chain counted as allocated");
    check(OPS->prepare_unmount(&mnt) == 0, "its clean unmount");
    unmount_image();
    check(memcmp(image_before + 512, image + 512, (unsigned long)image_sectors * 512 - 512) == 0,
          "and nothing but the boot state written: the chain's bytes are intact");
    {
        int rc = run_check(check_out, sizeof(check_out));

        check(rc != 0 && strstr(check_out, "lost: 2 cluster(s)") && findings_allowed(check_out),
              "which the walk counts as lost and nothing else");
    }

    for (f = 0; f < sizeof(faults) / sizeof(faults[0]); f++) {
        char what[200];

        snprintf(what, sizeof(what), "%s mounts read-only", faults[f][0]);
        dirty_mount(faults[f][0], 1, what);
        snprintf(what, sizeof(what), "%s names its fault: %s", faults[f][0], faults[f][1]);
        check(strstr(last_klog, faults[f][1]) != 0, what);
        if (!strstr(last_klog, faults[f][1])) {
            printf("exfat_hosttest: the check said instead: %s\n", last_klog);
        }
        check(memcmp(image_before, image, (unsigned long)image_sectors * 512) == 0,
              "and the image is untouched");
        unmount_image();
    }

    /* The recursive matrix: each repair's write a stop; the state left is
       one the walk allows, the files reachable. */
    for (f = 0; f < sizeof(repairs) / sizeof(repairs[0]); f++) {
        int nwrites;

        load(repairs[f]);
        stop_after = -1;
        writes_made = 0;
        if (mount_rw() != 0) {
            check(0, "the repair matrix's volume mounts");
            return;
        }
        nwrites = writes_made;
        OPS->put(&mnt, mnt.root);
        OPS->unmount(&mnt);
        for (k = 1; k < nwrites; k++) {
            char what[160];

            load(repairs[f]);
            writes_made = 0;
            stop_after = k;
            (void)mount_rw(); /* the check's write fails at the stop: read-only, or refused */
            if (mnt.root) {
                OPS->put(&mnt, mnt.root);
                OPS->unmount(&mnt);
            }
            run_check(check_out, sizeof(check_out));
            load_dump();
            snprintf(what, sizeof(what),
                     "%s stopped after repair write %d of %d leaves a state the walk allows",
                     repairs[f], k, nwrites);
            check(findings_allowed(check_out), what);
            if (!findings_allowed(check_out)) {
                printf("exfat_hosttest: the check said:\n%s", check_out);
            }
            snprintf(what, sizeof(what),
                     "%s stopped after repair write %d of %d still reads its files", repairs[f], k,
                     nwrites);
            if (mount_image(VFS_MS_RDONLY, "") == 0) {
                long got;

                n = walk("/chained.bin", &err);
                got = n ? read_all(n, back, sizeof(back)) : -1;
                if (n) {
                    OPS->put(&mnt, n);
                }
                /* The set the checksum repair rewrites reads once the
                   repair is on the disk, and its bytes are never wrong. */
                n = walk("/hello.txt", &err);
                if (n) {
                    long h = read_all(n, back, sizeof(back));

                    if (h != 12 || memcmp(back, "hello exfat\n", 12) != 0) {
                        got = -1;
                    }
                    OPS->put(&mnt, n);
                }
                check(got >= 4096, what);
                unmount_image();
            } else {
                check(0, what);
            }
        }
    }
}

int main(void)
{
    proc_a.pid = 1;
    devmodel_init(&model, medium, image, IMG_BYTES);
    well_formed_rows("", "the held up-case table");
    well_formed_rows("--big-upcase", "the streamed up-case table");
    well_formed_rows("--uncompressed-upcase", "the indexed up-case table");
    damaged_rows();
    external_rows();
    write_rows();
    durability_rows();
    matrix_pass();
    dirty_rows();
    if (fails == 0) {
        printf("exfat_hosttest: OK\n");
        return 0;
    }
    printf("exfat_hosttest: %d failure(s)\n", fails);
    return 1;
}
