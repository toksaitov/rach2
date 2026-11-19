/**
 * tmpfs_hosttest.c: tmpfs, rach2/sys/fs/tmpfs.c, and the memory file under
 * each of its files, rach2/sys/mm/shmem.c.
 *
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 *
 * The test includes both sources whole, with a pool of host pages for the
 * frames. A frame comes filled with 0xA5, as a real one comes with what its
 * last user left, so a page that reads as zeros was zeroed by the code under
 * test. The rows run in order on one mount, each on the names the earlier
 * rows left, and every frame must be back in the pool after the unmount.
 */

#include "fs/tmpfs.c"
#include "mm/shmem.c"

extern int printf(const char *format, ...);
extern void *malloc(unsigned long size);
extern void *calloc(unsigned long count, unsigned long size);
extern void free(void *ptr);

static int fails;

#define CHECK(cond)                                                         \
    do {                                                                    \
        if (!(cond)) {                                                      \
            printf("tmpfs-hosttest: fail: line %d: %s\n", __LINE__, #cond); \
            fails++;                                                        \
        }                                                                   \
    } while (0)

static process_t process;
struct percpu cpu_table[NR_CPUS] = {{.current = &process}};

static unsigned service_points;

/** Count the scheduling points of a sparse cache walk. */
void sched_service_point(void)
{
    service_points++;
}

/* The give-ways of the frees, and a check each runs while `at_give_way`
 * is set. */
static unsigned give_ways;
static void (*at_give_way)(void);

/** Count a give-way of a free and run the row's check there, if it set
 *  one. */
void sched_give_way(void)
{
    give_ways++;
    if (at_give_way) {
        at_give_way();
    }
}

/* When positive, the allocation that counts it down to zero fails. */
static int heap_fails_in;

/** Whether this allocation fails, as `heap_fails_in` counts. */
static int heap_refuses(void)
{
    return heap_fails_in > 0 && --heap_fails_in == 0;
}

/** The host's allocation, or NULL when `heap_refuses`. */
void *kmalloc(size_t n)
{
    return heap_refuses() ? NULL : malloc(n);
}

/** The host's zeroed allocation, or NULL when `heap_refuses`. */
void *kzalloc(size_t n)
{
    return heap_refuses() ? NULL : calloc(1, n);
}

/** Free what `kmalloc` or `kzalloc` gave. */
void kfree(void *p)
{
    free(p);
}

/** The time of day, which stands at 1000 seconds. */
void get_current_time(int64_t *seconds, int64_t *nanoseconds)
{
    *seconds = 1000;
    *nanoseconds = 0;
}

/* The RLIMIT_FSIZE the stub of `vfs_newsize_ok` judges by, all ones for
 * none, and the SIGXFSZ it has counted in place of sending. */
static uint64_t fsize_limit = ~(uint64_t)0;
static int fsize_signals;

/** `vfs_newsize_ok` against `fsize_limit`: -EFBIG, with a SIGXFSZ counted,
 * for an `end` past both the size and the limit, else 0. */
int32_t vfs_newsize_ok(uint64_t size, uint64_t end)
{
    if (end <= size || end <= fsize_limit) {
        return 0;
    }
    fsize_signals++;
    return -EFBIG;
}

/* The frames: page i of `pool` is the frame at physical address
 * (i + 1) * PAGE_SIZE, so that no frame is at 0, and `refs[i]` counts its
 * references, 0 for a free frame. */
#define POOL 64
static unsigned char pool[POOL][PAGE_SIZE];
static int refs[POOL];

/** The index in `pool` of the frame at `phys`. */
static int frame_index(uintptr_t phys)
{
    return (int)(phys / PAGE_SIZE) - 1;
}

/** A free frame of the pool, filled with 0xA5 and with one reference.
 *  Returns its physical address, or 0 when the pool has none free. */
uintptr_t alloc_user_frame(void)
{
    int i;

    for (i = 0; i < POOL; i++) {
        if (refs[i] == 0) {
            refs[i] = 1;
            k_memset(pool[i], 0xA5, PAGE_SIZE);
            return (uintptr_t)(i + 1) * PAGE_SIZE;
        }
    }
    return 0;
}

/** Drop a reference to the frame at `phys`. */
void free_frame(uintptr_t phys)
{
    refs[frame_index(phys)]--;
}

/** Take a reference to the frame at `phys`. */
void frame_ref(uintptr_t phys)
{
    refs[frame_index(phys)]++;
}

/** The pool page of the frame at `phys`. */
void *kmap_temp(uintptr_t phys)
{
    return pool[frame_index(phys)];
}

/** End a map `kmap_temp` made, which has nothing to undo. */
void kunmap_temp(void *va)
{
    (void)va;
}

/** The pool page of the frame at `phys`, as `kmap_temp` gives it. */
void *kmap_cpu(uintptr_t phys, uint32_t *flags)
{
    *flags = 0;
    return pool[frame_index(phys)];
}

/** End a map `kmap_cpu` made, which has nothing to undo. */
void kunmap_cpu(void *va, uint32_t flags)
{
    (void)va;
    (void)flags;
}

/** Zero the frame at `phys`. Returns 0. */
int paging_zero_frame(uintptr_t phys)
{
    k_memset(pool[frame_index(phys)], 0, PAGE_SIZE);
    return 0;
}

/** The machine's frames, the pool's. */
size_t frame_total_count(void)
{
    return POOL;
}

/** The frames of the pool with no reference. */
size_t frame_free_count(void)
{
    size_t n = 0;
    int i;

    for (i = 0; i < POOL; i++) {
        n += refs[i] == 0;
    }
    return n;
}

/** The frames of the pool with a reference. */
static int frames_in_use(void)
{
    return POOL - (int)frame_free_count();
}

/* What `filemap_shmem_cut` saw: the cuts, the first page of the last, and
 * the frames a mapping maps by page, 0 for none. */
static int cuts;
static uint32_t cut_first;
static uintptr_t mapped[4];

/** The mapping side's part, which a truncation of mapped memory calls before
 *  it frees the pages: count the cut, record `first`, and let go every page
 *  of `mapped` from `first` on, as the regions' page tables would. */
void filemap_shmem_cut(struct shmem *sh, uint32_t first)
{
    int i;

    (void)sh;
    cuts++;
    cut_first = first;
    for (i = 0; i < 4; i++) {
        if (mapped[i] && (uint32_t)i >= first) {
            free_frame(mapped[i]);
            mapped[i] = 0;
        }
    }
}

static struct vfs_mount mnt;

/** Create `name` of `mode` in `dir`, NULL for the root, which must succeed.
 *  Returns the node, held, or NULL. */
static void *make(void *dir, const char *name, uint32_t mode)
{
    void *n = NULL;

    CHECK(tmpfs_create(&mnt, dir, name, mode, &n) == 0);
    return n;
}

/** The node `name` in `dir`, NULL for the root, held, or NULL when there is
 *  none. */
static void *look(void *dir, const char *name)
{
    void *n = NULL;

    return tmpfs_lookup(&mnt, dir, name, &n) == 0 ? n : NULL;
}

/** Compare `n` bytes as memcmp does; the kernel's string library has no
 *  compare. */
static int k_memcmp(const void *a, const void *b, uint32_t n)
{
    const unsigned char *x = (const unsigned char *)a;
    const unsigned char *y = (const unsigned char *)b;

    while (n-- > 0) {
        if (*x != *y) {
            return *x - *y;
        }
        x++;
        y++;
    }
    return 0;
}

/** Whether the strings `a` and `b` are equal. */
static int same(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/**
 * A file's bytes, in the file "a". A write at 0 and one across the end of
 * page 1 take the frames of pages 0 and 1 and none for the hole between.
 * statfs counts those two in use, and not a frame held elsewhere, against
 * the mount's size, half the machine's frames by default. The bytes read
 * back with the hole as zeros, and a read at the end reads nothing. A
 * truncation to 3 bytes frees a page, and a growth to 10 reads zeros past
 * the 3.
 */
static void bytes(void)
{
    struct vfs_stat st;
    char buf[16];
    void *f = make(NULL, "a", 0644);

    CHECK(tmpfs_getattr(&mnt, f, &st) == 0 && st.mode == (T_IFREG | 0644u) && st.nlink == 1 &&
          st.size == 0);
    CHECK(tmpfs_write(&mnt, f, "hello", 5, 0) == 5);
    CHECK(tmpfs_write(&mnt, f, "xy", 2, 8190) == 2);
    CHECK(tmpfs_getattr(&mnt, f, &st) == 0 && st.size == 8192);
    CHECK(frames_in_use() == 2);
    {
        struct vfs_statfs sf;
        uintptr_t other = alloc_user_frame();

        CHECK(tmpfs_statfs(&mnt, &sf) == 0 && sf.blocks == POOL / 2 && sf.blocks - sf.bfree == 2);
        free_frame(other);
    }
    CHECK(tmpfs_read(&mnt, f, buf, 5, 0) == 5 && k_memcmp(buf, "hello", 5) == 0);
    CHECK(tmpfs_read(&mnt, f, buf, 4, 8188) == 4 && buf[0] == 0 && buf[1] == 0 && buf[2] == 'x');
    CHECK(tmpfs_read(&mnt, f, buf, 4, 8192) == 0);
    CHECK(tmpfs_truncate(&mnt, f, 3) == 0 && frames_in_use() == 1);
    CHECK(tmpfs_truncate(&mnt, f, 10) == 0);
    CHECK(tmpfs_read(&mnt, f, buf, 10, 0) == 10 && k_memcmp(buf, "hel\0\0\0\0\0\0\0", 10) == 0);
    tmpfs_put(&mnt, f);
}

/** Whether the `n` bytes at `p` are all zero. */
static int all_zero(const char *p, size_t n)
{
    while (n-- > 0) {
        if (*p++ != 0) {
            return 0;
        }
    }
    return 1;
}

/** A file whose first pages are a hole reads them as zeros into a buffer
 *  that held something else, and a write and a read across a page boundary
 *  move every byte: the write takes two frames, and the read returns the
 *  bytes of both pages and the hole's zeros after them. */
static void sparse(void)
{
    static char buf[PAGE_SIZE + 16];
    void *f = make(NULL, "s", 0644);
    int frames = frames_in_use();
    struct vfs_cachestat cs;

    CHECK(tmpfs_write(&mnt, f, "end", 3, 3 * PAGE_SIZE) == 3);
    CHECK(frames_in_use() == frames + 1);
    k_memset(buf, 0x5A, sizeof(buf));
    CHECK(tmpfs_read(&mnt, f, buf, PAGE_SIZE, PAGE_SIZE) == PAGE_SIZE && all_zero(buf, PAGE_SIZE));
    CHECK(tmpfs_write(&mnt, f, "abcdefgh", 8, 2 * PAGE_SIZE - 4) == 8);
    CHECK(frames_in_use() == frames + 3);
    k_memset(buf, 0x5A, sizeof(buf));
    CHECK(tmpfs_read(&mnt, f, buf, 16, 2 * PAGE_SIZE - 8) == 16 && all_zero(buf, 4) &&
          k_memcmp(buf + 4, "abcdefgh", 8) == 0 && all_zero(buf + 12, 4));
    k_memset(buf, 0x5A, sizeof(buf));
    CHECK(tmpfs_read(&mnt, f, buf, PAGE_SIZE + 16, 2 * PAGE_SIZE - 4) == PAGE_SIZE + 7 &&
          k_memcmp(buf, "abcdefgh", 8) == 0 && all_zero(buf + 8, PAGE_SIZE - 4) &&
          k_memcmp(buf + PAGE_SIZE + 4, "end", 3) == 0);
    CHECK(tmpfs_unlink(&mnt, NULL, "s") == 0);
    tmpfs_put(&mnt, f);
    CHECK(frames_in_use() == frames);
    f = make(NULL, "cache", 0644);
    CHECK(tmpfs_write(&mnt, f, "x", 1, 0) == 1);
    CHECK(tmpfs_truncate(&mnt, f, SHMEM_MAX_SIZE) == 0);
    k_memset(&cs, 0, sizeof(cs));
    service_points = 0;
    tmpfs_cachestat(&mnt, f, 0, 0xFFFFFFFEu, &cs);
    CHECK(cs.cache == 1 && cs.dirty == 0 && service_points == 1);
    CHECK(tmpfs_unlink(&mnt, NULL, "cache") == 0);
    tmpfs_put(&mnt, f);
    CHECK(frames_in_use() == frames);
}

/**
 * Directories. A mkdir of a name that exists is EEXIST, and a directory
 * counts its subdirectories in its link count. An rmdir of a directory with
 * a name in it is ENOTEMPTY, an unlink of a directory EISDIR and an rmdir of
 * a file ENOTDIR. ".." of a subdirectory is the root, and an rmdir takes the
 * root's link count back to 2.
 */
static void directories(void)
{
    struct vfs_stat st;
    void *root = ((struct tmpfs_mount *)mnt.priv)->root;
    void *d, *f;

    CHECK(tmpfs_mkdir(&mnt, NULL, "d", 0755) == 0);
    CHECK(tmpfs_mkdir(&mnt, NULL, "d", 0755) == -EEXIST);
    CHECK(tmpfs_getattr(&mnt, root, &st) == 0 && st.nlink == 3);
    d = look(NULL, "d");
    f = make(d, "f", 0600);
    CHECK(tmpfs_rmdir(&mnt, NULL, "d") == -ENOTEMPTY);
    CHECK(tmpfs_unlink(&mnt, NULL, "d") == -EISDIR);
    CHECK(tmpfs_rmdir(&mnt, d, "f") == -ENOTDIR);
    CHECK(tmpfs_unlink(&mnt, d, "f") == 0);
    tmpfs_put(&mnt, f);
    CHECK(look(d, "..") == root);
    tmpfs_put(&mnt, root);
    tmpfs_put(&mnt, d);
    CHECK(tmpfs_rmdir(&mnt, NULL, "d") == 0);
    CHECK(tmpfs_getattr(&mnt, root, &st) == 0 && st.nlink == 2);
    CHECK(look(NULL, "d") == NULL);
}

/**
 * Hard links and rename. A hard link keeps the bytes of "a" after its first
 * name goes, and a rename moves the name to "c". A rename is EINVAL for a
 * directory moved under itself, EISDIR for a file over a directory, ENOTDIR
 * for a directory over a file and ENOTEMPTY for a directory over one that
 * is not empty. A directory's path is written back, and is ENAMETOOLONG in
 * a buffer too small for it; a regular file has no parent path and gives
 * ENOTDIR. A rename over "c" replaces it, and the
 * replaced node, still held, reads its bytes with a link count of 0.
 */
static void links_and_renames(void)
{
    struct vfs_stat st;
    char buf[8];
    void *a = look(NULL, "a");
    void *x, *y, *held;

    CHECK(a != NULL && tmpfs_link(&mnt, NULL, "b", a) == 0);
    CHECK(tmpfs_getattr(&mnt, a, &st) == 0 && st.nlink == 2);
    CHECK(tmpfs_unlink(&mnt, NULL, "a") == 0);
    CHECK(tmpfs_getattr(&mnt, a, &st) == 0 && st.nlink == 1);
    tmpfs_put(&mnt, a);
    CHECK(tmpfs_rename(&mnt, NULL, "b", NULL, "c") == 0);
    CHECK(look(NULL, "b") == NULL);
    held = look(NULL, "c");
    CHECK(held != NULL && tmpfs_read(&mnt, held, buf, 3, 0) == 3 && k_memcmp(buf, "hel", 3) == 0);

    CHECK(tmpfs_mkdir(&mnt, NULL, "x", 0755) == 0);
    x = look(NULL, "x");
    CHECK(tmpfs_mkdir(&mnt, x, "y", 0755) == 0);
    y = look(x, "y");
    CHECK(tmpfs_rename(&mnt, NULL, "x", y, "z") == -EINVAL);
    CHECK(tmpfs_rename(&mnt, NULL, "c", NULL, "x") == -EISDIR);
    CHECK(tmpfs_rename(&mnt, x, "y", NULL, "c") == -ENOTDIR);
    CHECK(tmpfs_mkdir(&mnt, NULL, "w", 0755) == 0);
    CHECK(tmpfs_rename(&mnt, NULL, "w", NULL, "x") == -ENOTEMPTY);
    {
        char path[32];

        CHECK(tmpfs_path(&mnt, y, path, sizeof(path)) == 0 && same(path, "/x/y"));
        CHECK(tmpfs_path(&mnt, y, path, 4) == -ENAMETOOLONG);
        CHECK(tmpfs_path(&mnt, held, path, sizeof(path)) == -ENOTDIR);
    }
    {
        void *r = make(NULL, "r", 0644);

        CHECK(tmpfs_write(&mnt, r, "new", 3, 0) == 3);
        tmpfs_put(&mnt, r);
        CHECK(tmpfs_rename(&mnt, NULL, "r", NULL, "c") == 0);
        CHECK(look(NULL, "r") == NULL);
    }
    CHECK(tmpfs_read(&mnt, held, buf, 3, 0) == 3 && k_memcmp(buf, "hel", 3) == 0);
    CHECK(tmpfs_getattr(&mnt, held, &st) == 0 && st.nlink == 0);
    tmpfs_put(&mnt, held);
    tmpfs_put(&mnt, y);
    tmpfs_put(&mnt, x);
}

/**
 * The other kinds of node. A symbolic link reads back its target and has
 * its length as its size; a FIFO keeps its type and has no device number; a
 * device node keeps its number, a second mknod of its name is EEXIST, and
 * an open of it is ENXIO. A create of a directory is EINVAL.
 */
static void kinds(void)
{
    struct vfs_stat st;
    char buf[8];
    void *handle;
    void *s, *p, *c;

    CHECK(tmpfs_symlink(&mnt, NULL, "s", "c") == 0);
    s = look(NULL, "s");
    CHECK(tmpfs_readlink(&mnt, s, buf, sizeof(buf)) == 1 && buf[0] == 'c');
    CHECK(tmpfs_getattr(&mnt, s, &st) == 0 && (st.mode & T_IFMT) == T_IFLNK && st.size == 1);
    CHECK(tmpfs_path(&mnt, s, buf, sizeof(buf)) == -ENOTDIR);
    p = make(NULL, "p", T_IFIFO | 0600);
    CHECK(tmpfs_getattr(&mnt, p, &st) == 0 && (st.mode & T_IFMT) == T_IFIFO && st.rdev == 0);
    c = NULL;
    CHECK(tmpfs_mknod(&mnt, NULL, "dev", T_IFCHR | 0600, (1u << 8) | 3u, &c) == 0);
    CHECK(tmpfs_getattr(&mnt, c, &st) == 0 && st.mode == (T_IFCHR | 0600u) &&
          st.rdev == ((1u << 8) | 3u));
    CHECK(tmpfs_mknod(&mnt, NULL, "dev", T_IFBLK | 0600, 0, &handle) == -EEXIST);
    CHECK(tmpfs_open(&mnt, c, 0, &handle) == -ENXIO);
    CHECK(tmpfs_create(&mnt, NULL, "dir", T_IFDIR | 0755, &handle) == -EINVAL);
    tmpfs_put(&mnt, s);
    tmpfs_put(&mnt, p);
    tmpfs_put(&mnt, c);
}

/** The root's listing: ".", "..", then the names in the order they were
 *  made or renamed, each with its type. */
static void listing(void)
{
    static const char *want[] = {".", "..", "x", "w", "c", "s", "p", "dev"};
    static const uint32_t types[] = {T_IFDIR, T_IFDIR, T_IFDIR, T_IFDIR,
                                     T_IFREG, T_IFLNK, T_IFIFO, T_IFCHR};
    struct vfs_dirent ent;
    void *root = ((struct tmpfs_mount *)mnt.priv)->root;
    uint32_t cursor = 0;
    unsigned i;

    for (i = 0; i < sizeof(want) / sizeof(want[0]); i++) {
        CHECK(tmpfs_readdir(&mnt, root, &cursor, &ent) == 1 && same(ent.name, want[i]) &&
              ent.type == (int)VFS_DT_OF(types[i]));
    }
    CHECK(tmpfs_readdir(&mnt, root, &cursor, &ent) == 0);
}

/**
 * A name removed during a listing moves no other: after ".", ".." and "a",
 * with "a" removed, the listing goes on with "b" and "c".
 */
static void listing_removal(void)
{
    static const char *names[] = {"a", "b", "c"};
    struct vfs_dirent ent;
    uint32_t cursor = 0;
    unsigned i;
    void *d;

    CHECK(tmpfs_mkdir(&mnt, NULL, "ls", 0755) == 0);
    d = look(NULL, "ls");
    for (i = 0; i < 3; i++) {
        void *f = make(d, names[i], 0644);

        if (f) {
            tmpfs_put(&mnt, f);
        }
    }
    for (i = 0; i < 3; i++) {
        CHECK(tmpfs_readdir(&mnt, d, &cursor, &ent) == 1);
    }
    CHECK(same(ent.name, "a") && tmpfs_unlink(&mnt, d, "a") == 0);
    CHECK(tmpfs_readdir(&mnt, d, &cursor, &ent) == 1 && same(ent.name, "b"));
    CHECK(tmpfs_readdir(&mnt, d, &cursor, &ent) == 1 && same(ent.name, "c"));
    CHECK(tmpfs_readdir(&mnt, d, &cursor, &ent) == 0);
    tmpfs_put(&mnt, d);
}

/**
 * The pages of a mapping. The root has no memory file. A shared mapping's
 * page takes a reference of its own. A page at or past the most a file may
 * have, 2^32 - 1 pages, is 0 and does not grow the table, and the last page
 * below it is given. A truncation to 0 drops the file's references and
 * leaves the mapping's.
 */
static void mapping(void)
{
    const uint32_t most = (uint32_t)(SHMEM_MAX_SIZE / PAGE_SIZE);
    void *f = look(NULL, "c");
    struct shmem *sh = tmpfs_shmem(f);
    uintptr_t phys, last;
    uint32_t npages;

    CHECK(sh != NULL && tmpfs_shmem(((struct tmpfs_mount *)mnt.priv)->root) == NULL);
    phys = shmem_map_page(sh, 0);
    CHECK(phys != 0 && refs[frame_index(phys)] == 2);
    npages = sh->npages;
    CHECK(shmem_map_page(sh, 0xFFFFFFFFu) == 0 && shmem_map_page(sh, most) == 0 &&
          sh->npages == npages);
    last = shmem_map_page(sh, most - 1u);
    CHECK(last != 0 && sh->npages == most && refs[frame_index(last)] == 2);
    CHECK(tmpfs_truncate(&mnt, f, 0) == 0 && refs[frame_index(phys)] == 1 &&
          refs[frame_index(last)] == 1);
    free_frame(phys);
    free_frame(last);
    tmpfs_put(&mnt, f);
}

/** A truncation of memory a mapping maps takes the pages it frees out of
 *  the mapping first, from the first page past the new end, so that after
 *  the file grows again a write and the mapping's next touch meet in one
 *  page. Memory no mapping maps is truncated without the mapping side. */
static void truncation(void)
{
    void *f = make(NULL, "t", 0644);
    struct shmem *sh = tmpfs_shmem(f);

    CHECK(sh != NULL && tmpfs_write(&mnt, f, "abcd", 4, 3 * PAGE_SIZE) == 4);
    sh->maps = 1;
    mapped[0] = shmem_map_page(sh, 0);
    mapped[3] = shmem_map_page(sh, 3);
    cuts = 0;
    sh->size = 0; /* a disk read-around may not have sampled the size */
    CHECK(tmpfs_truncate(&mnt, f, PAGE_SIZE + 1) == 0 && cuts == 1 && cut_first == 2);
    CHECK(mapped[0] != 0 && mapped[3] == 0 && refs[frame_index(mapped[0])] == 2);
    CHECK(tmpfs_truncate(&mnt, f, 0) == 0 && cuts == 2 && cut_first == 0 && mapped[0] == 0);
    CHECK(tmpfs_write(&mnt, f, "x", 1, 3 * PAGE_SIZE) == 1);
    mapped[3] = shmem_map_page(sh, 3);
    CHECK(mapped[3] != 0 && pool[frame_index(mapped[3])][0] == 'x');
    free_frame(mapped[3]);
    mapped[3] = 0;
    sh->maps = 0;
    CHECK(tmpfs_truncate(&mnt, f, 0) == 0 && cuts == 2);
    tmpfs_put(&mnt, f);
}

/**
 * A memfd's seals. A memfd has its name and no seals. F_SEAL_SHRINK makes a
 * shrink, by truncate or by setattr, EPERM and leaves a growth; F_SEAL_GROW
 * makes a write past the end and a growth EPERM and leaves a write inside;
 * F_SEAL_FUTURE_WRITE makes a write EPERM; an unknown seal is EINVAL; and
 * F_SEAL_SEAL makes another seal EPERM. A named file has no name of a memfd
 * and starts sealed against sealing. F_SEAL_EXEC on a file with execute
 * bits seals it against writing too, and makes a change of its execute bits
 * EPERM while a change of another bit is made.
 */
static void seals(void)
{
    struct vfs_stat st;
    char buf[8];
    void *f = NULL;

    CHECK(tmpfs_memfd(&mnt, "blob", 0777, 0, &f) == 0 && same(tmpfs_label(f), "blob") &&
          tmpfs_seals(f) == 0);
    CHECK(tmpfs_write(&mnt, f, "sealed", 6, 0) == 6);
    CHECK(tmpfs_add_seals(f, TMPFS_SEAL_SHRINK) == 0);
    CHECK(tmpfs_truncate(&mnt, f, 3) == -EPERM && tmpfs_truncate(&mnt, f, 10) == 0);
    st.size = 2;
    CHECK(tmpfs_setattr(&mnt, f, VFS_SET_SIZE, &st) == -EPERM);
    CHECK(tmpfs_add_seals(f, TMPFS_SEAL_GROW) == 0);
    CHECK(tmpfs_write(&mnt, f, "x", 1, 10) == -EPERM && tmpfs_write(&mnt, f, "S", 1, 0) == 1);
    CHECK(tmpfs_truncate(&mnt, f, 11) == -EPERM);
    CHECK(tmpfs_add_seals(f, TMPFS_SEAL_FUTURE_WRITE) == 0 &&
          tmpfs_write(&mnt, f, "s", 1, 0) == -EPERM);
    CHECK(tmpfs_add_seals(f, 0x40) == -EINVAL);
    CHECK(tmpfs_add_seals(f, TMPFS_SEAL_SEAL) == 0 &&
          tmpfs_add_seals(f, TMPFS_SEAL_WRITE) == -EPERM);
    CHECK(tmpfs_read(&mnt, f, buf, 6, 0) == 6 && k_memcmp(buf, "Sealed", 6) == 0);
    tmpfs_put(&mnt, f);

    f = make(NULL, "named", 0644);
    CHECK(tmpfs_seals(f) == TMPFS_SEAL_SEAL && tmpfs_add_seals(f, TMPFS_SEAL_WRITE) == -EPERM &&
          tmpfs_label(f) == NULL);
    tmpfs_put(&mnt, f);
    CHECK(tmpfs_unlink(&mnt, NULL, "named") == 0);

    CHECK(tmpfs_memfd(&mnt, "exec", 0755, 0, &f) == 0 && tmpfs_add_seals(f, TMPFS_SEAL_EXEC) == 0 &&
          (tmpfs_seals(f) & TMPFS_SEAL_WRITE));
    st.mode = 0644;
    CHECK(tmpfs_setattr(&mnt, f, VFS_SET_MODE, &st) == -EPERM);
    st.mode = 0755 | 04000;
    CHECK(tmpfs_setattr(&mnt, f, VFS_SET_MODE, &st) == 0);
    tmpfs_put(&mnt, f);
}

/**
 * A removed directory that is still held keeps its removed parent: a listing
 * of it is ENOENT from any cursor, its ".." is walked to the parent, and both
 * go with the last hold.
 */
static void removed_parent(void)
{
    struct vfs_dirent ent;
    uint32_t cursor = 0;
    void *p, *c;

    CHECK(tmpfs_mkdir(&mnt, NULL, "rp", 0755) == 0);
    p = look(NULL, "rp");
    CHECK(tmpfs_mkdir(&mnt, p, "rc", 0755) == 0);
    c = look(p, "rc");
    tmpfs_put(&mnt, p);
    CHECK(tmpfs_readdir(&mnt, c, &cursor, &ent) == 1 && cursor == 1);
    CHECK(tmpfs_rmdir(&mnt, p, "rc") == 0 && tmpfs_rmdir(&mnt, NULL, "rp") == 0);
    CHECK(tmpfs_readdir(&mnt, c, &cursor, &ent) == -ENOENT && cursor == 1);
    cursor = 0;
    CHECK(tmpfs_readdir(&mnt, c, &cursor, &ent) == -ENOENT && cursor == 0);
    CHECK(look(c, "..") == p);
    tmpfs_put(&mnt, p);
    tmpfs_put(&mnt, c);
}

/**
 * A directory removed while it is held reports no links and takes no name:
 * a create, a mknod, a mkdir, a symbolic link, a hard link and a rename into
 * it are each ENOENT, and the file "rf" keeps its name in the root.
 */
static void removed_directory(void)
{
    struct vfs_stat st;
    void *d, *f, *again, *n = NULL;

    CHECK(tmpfs_mkdir(&mnt, NULL, "rd", 0755) == 0);
    d = look(NULL, "rd");
    f = make(NULL, "rf", 0644);
    CHECK(tmpfs_rmdir(&mnt, NULL, "rd") == 0);
    CHECK(tmpfs_getattr(&mnt, d, &st) == 0 && st.nlink == 0);
    CHECK(tmpfs_create(&mnt, d, "f", 0644, &n) == -ENOENT && n == NULL);
    CHECK(tmpfs_mknod(&mnt, d, "p", T_IFIFO | 0644, 0, &n) == -ENOENT && n == NULL);
    CHECK(tmpfs_mkdir(&mnt, d, "s", 0755) == -ENOENT);
    CHECK(tmpfs_symlink(&mnt, d, "l", "rf") == -ENOENT);
    CHECK(tmpfs_link(&mnt, d, "h", f) == -ENOENT);
    CHECK(tmpfs_rename(&mnt, NULL, "rf", d, "m") == -ENOENT);
    CHECK(look(d, "f") == NULL && look(d, "m") == NULL);
    again = look(NULL, "rf");
    CHECK(again == f);
    if (again) {
        tmpfs_put(&mnt, again);
    }
    CHECK(tmpfs_unlink(&mnt, NULL, "rf") == 0);
    tmpfs_put(&mnt, f);
    tmpfs_put(&mnt, d);
}

/**
 * A rename the heap cannot serve is ENOSPC and changes nothing, whether the
 * allocation of its entry or of its name fails: a directory with a file in
 * it keeps its old name and the file, and the new name is not made.
 */
static void rename_refused(void)
{
    void *d, *f, *again;

    CHECK(tmpfs_mkdir(&mnt, NULL, "old", 0755) == 0);
    d = look(NULL, "old");
    f = make(d, "inside", 0644);
    heap_fails_in = 1;
    CHECK(tmpfs_rename(&mnt, NULL, "old", NULL, "a-longer-name") == -ENOSPC);
    heap_fails_in = 2;
    CHECK(tmpfs_rename(&mnt, NULL, "old", NULL, "a-longer-name") == -ENOSPC);
    heap_fails_in = 0;
    CHECK(look(NULL, "a-longer-name") == NULL);
    again = look(NULL, "old");
    CHECK(again == d);
    if (again) {
        tmpfs_put(&mnt, again);
    }
    again = look(d, "inside");
    CHECK(again == f);
    if (again) {
        tmpfs_put(&mnt, again);
    }
    tmpfs_put(&mnt, f);
    tmpfs_put(&mnt, d);
}

/**
 * A mount's size= is the most pages its files hold: a write the limit stops
 * is short and then ENOSPC, statfs gives the limit as the blocks, and a
 * truncation gives the pages back. A remount takes a new size= that the
 * files' pages fit in, and is EINVAL for a smaller one. size=0 is no limit,
 * a percentage is of the machine's frames, and a value that is no number
 * fails the mount with EINVAL. A file reaches past 1 GiB with a hole before
 * its one page, and stops a page short of 16 TiB.
 */
static void sizes(void)
{
    static char buf[3 * PAGE_SIZE];
    struct vfs_mount m2;
    struct vfs_statfs sf;
    void *f = NULL;

    k_memset(&m2, 0, sizeof(m2));
    CHECK(tmpfs_mount(&m2, NULL, 0, "size=8k") == 0);
    CHECK(tmpfs_create(&m2, NULL, "big", 0644, &f) == 0);
    CHECK(tmpfs_write(&m2, f, buf, sizeof(buf), 0) == 2 * PAGE_SIZE);
    CHECK(tmpfs_write(&m2, f, buf, 1, 2 * PAGE_SIZE) == -ENOSPC);
    CHECK(tmpfs_statfs(&m2, &sf) == 0 && sf.blocks == 2 && sf.bfree == 0);
    CHECK(tmpfs_remount(&m2, 0, "size=4k") == -EINVAL);
    CHECK(tmpfs_remount(&m2, 0, "mode=0700,size=12k") == 0 && tmpfs_statfs(&m2, &sf) == 0 &&
          sf.blocks == 3 && sf.bfree == 1);
    CHECK(tmpfs_write(&m2, f, buf, 1, 2 * PAGE_SIZE) == 1);
    CHECK(tmpfs_remount(&m2, 0, NULL) == 0 && tmpfs_remount(&m2, 0, "size=8k") == -EINVAL);
    CHECK(tmpfs_truncate(&m2, f, 0) == 0 && tmpfs_remount(&m2, 0, "size=8k") == 0 &&
          tmpfs_statfs(&m2, &sf) == 0 && sf.bfree == 2);
    CHECK(tmpfs_write(&m2, f, "x", 1, 0x40000000ull + 5) == 1);
    CHECK(tmpfs_read(&m2, f, buf, 6, 0x40000000ull) == 6 && buf[5] == 'x' && buf[0] == 0);
    CHECK(tmpfs_statfs(&m2, &sf) == 0 && sf.bfree == 1);
    CHECK(tmpfs_write(&m2, f, "x", 1, SHMEM_MAX_SIZE) == -EFBIG);
    tmpfs_put(&m2, f);
    tmpfs_unmount(&m2);
    CHECK(tmpfs_mount(&m2, NULL, 0, "mode=0700,size=0") == 0 && tmpfs_statfs(&m2, &sf) == 0 &&
          sf.blocks == POOL);
    tmpfs_unmount(&m2);
    CHECK(tmpfs_mount(&m2, NULL, 0, "size=50%") == 0 && tmpfs_statfs(&m2, &sf) == 0 &&
          sf.blocks == POOL / 2);
    tmpfs_unmount(&m2);
    CHECK(tmpfs_mount(&m2, NULL, 0, "size=lots") == -EINVAL);
}

/** Run the attribute call `op` of `name` on `n` with `size` bytes of `buf`
 *  and `flags`, as the VFS passes it. Returns what the type answers. */
static int xattr(void *n, int op, const char *name, void *buf, uint32_t size, int flags)
{
    struct vfs_xattr a;

    a.op = op;
    a.name = name;
    a.value = buf;
    a.size = size;
    a.flags = flags;
    return tmpfs_xattr(&mnt, n, &a);
}

/**
 * The inode flags: a new file has none and no generation; it keeps the
 * immutable, append-only, no-dump and no-atime flags it is given, refuses
 * any other with EOPNOTSUPP and keeps what it had, and a read of a file
 * with no-atime leaves its access time. A directory passes no-dump and
 * no-atime, and not the other two, to a node made in it.
 */
static void flags(void)
{
    struct vfs_stat st;
    char buf[4];
    void *f = make(NULL, "fl", 0644);
    void *d = NULL, *g;

    CHECK(tmpfs_getattr(&mnt, f, &st) == 0 && st.flags_known && !st.generation_known &&
          st.flags == 0);
    st.flags = VFS_FL_IMMUTABLE | FL_NOATIME;
    CHECK(tmpfs_setattr(&mnt, f, VFS_SET_FLAGS, &st) == 0);
    st.flags = 0x1u;
    CHECK(tmpfs_setattr(&mnt, f, VFS_SET_FLAGS, &st) == -EOPNOTSUPP);
    CHECK(tmpfs_getattr(&mnt, f, &st) == 0 && st.flags == (VFS_FL_IMMUTABLE | FL_NOATIME));
    CHECK(tmpfs_write(&mnt, f, "ab", 2, 0) == 2);
    st.atime_sec = 7;
    st.atime_nsec = 0;
    CHECK(tmpfs_setattr(&mnt, f, VFS_SET_ATIME, &st) == 0);
    CHECK(tmpfs_read(&mnt, f, buf, 2, 0) == 2 && tmpfs_getattr(&mnt, f, &st) == 0 &&
          st.atime_sec == 7);
    st.flags = 0;
    CHECK(tmpfs_setattr(&mnt, f, VFS_SET_FLAGS, &st) == 0);
    CHECK(tmpfs_read(&mnt, f, buf, 2, 0) == 2 && tmpfs_getattr(&mnt, f, &st) == 0 &&
          st.atime_sec != 7);
    CHECK(tmpfs_mkdir(&mnt, NULL, "fd", 0755) == 0 && (d = look(NULL, "fd")) != NULL);
    st.flags = VFS_FL_IMMUTABLE | VFS_FL_APPEND | FL_NODUMP | FL_NOATIME;
    CHECK(tmpfs_setattr(&mnt, d, VFS_SET_FLAGS, &st) == 0);
    g = make(d, "in", 0644);
    CHECK(tmpfs_getattr(&mnt, g, &st) == 0 && st.flags == (FL_NODUMP | FL_NOATIME));
    tmpfs_put(&mnt, g);
    CHECK(tmpfs_unlink(&mnt, d, "in") == 0);
    tmpfs_put(&mnt, d);
    CHECK(tmpfs_rmdir(&mnt, NULL, "fd") == 0);
    tmpfs_put(&mnt, f);
    CHECK(tmpfs_unlink(&mnt, NULL, "fl") == 0);
}

/**
 * Extended attributes: a value set, read whole, its length asked with no
 * buffer, and ERANGE for a buffer too small; EEXIST with XATTR_CREATE and
 * ENODATA with XATTR_REPLACE or a removal of a missing one; an empty value
 * kept; EINVAL for a bare prefix and EOPNOTSUPP for another namespace; the
 * names listed in the order they were made, the trusted ones only to a
 * privileged caller, and ERANGE for a list too small; a replaced value
 * keeps its place; and the node's attributes go with it.
 */
static void attributes(void)
{
    char buf[64];
    void *f = make(NULL, "xa", 0644);

    CHECK(xattr(f, VFS_XATTR_SET, "user.a", "hello", 5, 0) == 0);
    CHECK(xattr(f, VFS_XATTR_GET, "user.a", buf, 0, 0) == 5);
    CHECK(xattr(f, VFS_XATTR_GET, "user.a", buf, sizeof(buf), 0) == 5 &&
          k_memcmp(buf, "hello", 5) == 0);
    CHECK(xattr(f, VFS_XATTR_GET, "user.a", buf, 2, 0) == -ERANGE);
    CHECK(xattr(f, VFS_XATTR_SET, "user.a", "x", 1, 1) == -EEXIST);
    CHECK(xattr(f, VFS_XATTR_SET, "user.b", "x", 1, 2) == -ENODATA);
    CHECK(xattr(f, VFS_XATTR_REMOVE, "user.b", NULL, 0, 0) == -ENODATA);
    CHECK(xattr(f, VFS_XATTR_GET, "user.b", buf, sizeof(buf), 0) == -ENODATA);
    CHECK(xattr(f, VFS_XATTR_SET, "user.", "x", 1, 0) == -EINVAL);
    CHECK(xattr(f, VFS_XATTR_SET, "system.x", "x", 1, 0) == -EOPNOTSUPP);
    CHECK(xattr(f, VFS_XATTR_GET, "other", buf, sizeof(buf), 0) == -EOPNOTSUPP);
    CHECK(xattr(f, VFS_XATTR_SET, "trusted.t", "tt", 2, 1) == 0);
    CHECK(xattr(f, VFS_XATTR_SET, "security.s", buf, 0, 0) == 0);
    CHECK(xattr(f, VFS_XATTR_GET, "security.s", buf, sizeof(buf), 0) == 0);
    CHECK(xattr(f, VFS_XATTR_LIST, NULL, buf, 0, 0) == 28);
    CHECK(xattr(f, VFS_XATTR_LIST, NULL, buf, sizeof(buf), 0) == 28 &&
          k_memcmp(buf, "user.a\0trusted.t\0security.s\0", 28) == 0);
    CHECK(xattr(f, VFS_XATTR_LIST, NULL, buf, 27, 0) == -ERANGE);
    process.euid = 1000;
    CHECK(xattr(f, VFS_XATTR_LIST, NULL, buf, sizeof(buf), 0) == 18 &&
          k_memcmp(buf, "user.a\0security.s\0", 18) == 0);
    process.euid = 0;
    CHECK(xattr(f, VFS_XATTR_SET, "user.a", "bye", 3, 2) == 0);
    CHECK(xattr(f, VFS_XATTR_LIST, NULL, buf, sizeof(buf), 0) == 28 &&
          k_memcmp(buf, "user.a\0", 7) == 0);
    CHECK(xattr(f, VFS_XATTR_GET, "user.a", buf, sizeof(buf), 0) == 3 &&
          k_memcmp(buf, "bye", 3) == 0);
    CHECK(xattr(f, VFS_XATTR_REMOVE, "trusted.t", NULL, 0, 0) == 0);
    CHECK(xattr(f, VFS_XATTR_LIST, NULL, buf, 0, 0) == 18);
    tmpfs_put(&mnt, f);
    CHECK(tmpfs_unlink(&mnt, NULL, "xa") == 0);
}

/**
 * nr_inodes's space, on the mount `m2` of `inodes` with the file `b` and
 * two inodes free: an attribute takes its 40 bytes, name and value of it,
 * and one that does not fit is ENOSPC. A remount below what is used is
 * EINVAL, one above it gives the difference, and nr_inodes=0 lifts the
 * limit for good, after which statfs, whose record the VFS zeroes, gives no
 * count. Puts `b` and unmounts `m2`.
 */
static void inode_space(struct vfs_mount *m2, void *b)
{
    static char big[1000];
    struct vfs_xattr set = {VFS_XATTR_SET, "user.x", big, 1000, 0};
    struct vfs_xattr drop = {VFS_XATTR_REMOVE, "user.x", NULL, 0, 0};
    struct vfs_statfs sf;

    CHECK(tmpfs_xattr(m2, b, &set) == 0);
    CHECK(tmpfs_statfs(m2, &sf) == 0 && sf.ffree == 0);
    set.name = "user.y";
    CHECK(tmpfs_xattr(m2, b, &set) == -ENOSPC);
    CHECK(tmpfs_remount(m2, 0, "nr_inodes=2") == -EINVAL);
    CHECK(tmpfs_remount(m2, 0, "nr_inodes=8") == 0 && tmpfs_statfs(m2, &sf) == 0 && sf.files == 8 &&
          sf.ffree == (8u * 1024u - 2u * 1024u - 1046u) / 1024u);
    CHECK(tmpfs_xattr(m2, b, &drop) == 0);
    CHECK(tmpfs_statfs(m2, &sf) == 0 && sf.ffree == 6);
    k_memset(&sf, 0, sizeof(sf));
    CHECK(tmpfs_remount(m2, 0, "nr_inodes=0") == 0 && tmpfs_statfs(m2, &sf) == 0 && sf.files == 0 &&
          sf.ffree == 0);
    CHECK(tmpfs_remount(m2, 0, "nr_inodes=8") == -EINVAL);
    tmpfs_put(m2, b);
    CHECK(tmpfs_unlink(m2, NULL, "a2") == 0);
    tmpfs_unmount(m2);
}

/**
 * nr_inodes: the root takes one inode of four, each node one more, and a
 * second name of a file one, so a fifth is ENOSPC for a file, a directory,
 * a link and a hard link, and a removal, by unlink or by a rename over a
 * name, gives one back. Then `inode_space`.
 */
static void inodes(void)
{
    struct vfs_mount m2;
    struct vfs_statfs sf;
    void *a = NULL, *b = NULL, *c = NULL, *d = NULL;

    k_memset(&m2, 0, sizeof(m2));
    CHECK(tmpfs_mount(&m2, NULL, 0, "nr_inodes=4") == 0);
    CHECK(tmpfs_statfs(&m2, &sf) == 0 && sf.files == 4 && sf.ffree == 3);
    CHECK(tmpfs_create(&m2, NULL, "a", 0644, &a) == 0);
    CHECK(tmpfs_create(&m2, NULL, "b", 0644, &b) == 0);
    CHECK(tmpfs_create(&m2, NULL, "c", 0644, &c) == 0);
    CHECK(tmpfs_statfs(&m2, &sf) == 0 && sf.ffree == 0);
    CHECK(tmpfs_create(&m2, NULL, "d", 0644, &d) == -ENOSPC);
    CHECK(tmpfs_mkdir(&m2, NULL, "e", 0755) == -ENOSPC);
    CHECK(tmpfs_symlink(&m2, NULL, "f", "a") == -ENOSPC);
    CHECK(tmpfs_link(&m2, NULL, "a2", a) == -ENOSPC);
    tmpfs_put(&m2, c);
    CHECK(tmpfs_unlink(&m2, NULL, "c") == 0 && tmpfs_statfs(&m2, &sf) == 0 && sf.ffree == 1);
    CHECK(tmpfs_link(&m2, NULL, "a2", a) == 0 && tmpfs_statfs(&m2, &sf) == 0 && sf.ffree == 0);
    CHECK(tmpfs_unlink(&m2, NULL, "a") == 0 && tmpfs_statfs(&m2, &sf) == 0 && sf.ffree == 1);
    CHECK(tmpfs_rename(&m2, NULL, "b", NULL, "a2") == 0 && tmpfs_statfs(&m2, &sf) == 0 &&
          sf.ffree == 1);
    tmpfs_put(&m2, a);
    CHECK(tmpfs_statfs(&m2, &sf) == 0 && sf.ffree == 2);
    inode_space(&m2, b);
}

/**
 * The options of the limits: nr_inodes= takes a suffix of a power of 1024,
 * defaults to half the machine's pages, and is EINVAL when it is no number
 * or too large; size= sets no limit on a mount that has none.
 */
static void limit_options(void)
{
    struct vfs_mount m2;
    struct vfs_statfs sf;

    k_memset(&m2, 0, sizeof(m2));
    CHECK(tmpfs_mount(&m2, NULL, 0, "nr_inodes=1k") == 0 && tmpfs_statfs(&m2, &sf) == 0 &&
          sf.files == 1024);
    tmpfs_unmount(&m2);
    CHECK(tmpfs_mount(&m2, NULL, 0, "size=0") == 0 && tmpfs_statfs(&m2, &sf) == 0 &&
          sf.files == POOL / 2);
    CHECK(tmpfs_remount(&m2, 0, "size=8k") == -EINVAL);
    tmpfs_unmount(&m2);
    CHECK(tmpfs_mount(&m2, NULL, 0, "nr_inodes=lots") == -EINVAL);
    CHECK(tmpfs_mount(&m2, NULL, 0, "nr_inodes=4q") == -EINVAL);
    CHECK(tmpfs_mount(&m2, NULL, 0, "nr_inodes=99999999999999999999") == -EINVAL);
}

/**
 * fallocate: the range's pages made and the size grown to its end, or kept
 * with FALLOC_FL_KEEP_SIZE; a punched hole reads as zeros, frees the pages
 * it covers whole and keeps the size; a growth the grow seal forbids is
 * -EPERM, with the size kept or not, and a mode tmpfs does not take
 * -EOPNOTSUPP. Under RLIMIT_FSIZE an allocation past the size and the
 * limit is -EFBIG with SIGXFSZ, with the size kept or not, and allocates
 * nothing; a hole is punched past the limit, and a mode tmpfs does not
 * take is -EOPNOTSUPP with no signal.
 */
static void allocation(void)
{
    char buf[8];
    void *f = make(NULL, "fa", 0644);
    struct tmpfs_node *n = (struct tmpfs_node *)f;
    int frames = frames_in_use();

    CHECK(tmpfs_fallocate(&mnt, f, 0, 100, 2 * PAGE_SIZE) == 0 &&
          n->data.size == 100 + 2 * PAGE_SIZE);
    CHECK(frames_in_use() == frames + 3);
    CHECK(tmpfs_fallocate(&mnt, f, 0x01, 0, 5 * PAGE_SIZE) == 0 &&
          n->data.size == 100 + 2 * PAGE_SIZE && frames_in_use() == frames + 5);
    CHECK(tmpfs_write(&mnt, f, "abcdefgh", 8, PAGE_SIZE - 4) == 8);
    CHECK(tmpfs_fallocate(&mnt, f, 0x03, PAGE_SIZE - 2, PAGE_SIZE + 2) == 0);
    CHECK(tmpfs_read(&mnt, f, buf, 8, PAGE_SIZE - 4) == 8 && buf[0] == 'a' && buf[1] == 'b' &&
          buf[2] == 0 && buf[7] == 0);
    CHECK(frames_in_use() == frames + 4 && n->data.size == 100 + 2 * PAGE_SIZE);
    CHECK(tmpfs_fallocate(&mnt, f, 0x02, 0, PAGE_SIZE) == -EOPNOTSUPP);
    n->seals |= TMPFS_SEAL_GROW;
    CHECK(tmpfs_fallocate(&mnt, f, 0, 0, 9 * PAGE_SIZE) == -EPERM);
    CHECK(tmpfs_fallocate(&mnt, f, 0x01, 0, 9 * PAGE_SIZE) == -EPERM);
    n->seals &= ~TMPFS_SEAL_GROW;
    CHECK(tmpfs_unlink(&mnt, NULL, "fa") == 0);
    tmpfs_put(&mnt, f);
    CHECK(frames_in_use() == frames);

    f = make(NULL, "fl", 0644);
    n = (struct tmpfs_node *)f;
    fsize_limit = PAGE_SIZE;
    CHECK(tmpfs_fallocate(&mnt, f, 0x01, 0, 2 * PAGE_SIZE) == -EFBIG && fsize_signals == 1 &&
          frames_in_use() == frames);
    CHECK(tmpfs_fallocate(&mnt, f, 0, 0, 2 * PAGE_SIZE) == -EFBIG && fsize_signals == 2 &&
          n->data.size == 0 && frames_in_use() == frames);
    CHECK(tmpfs_fallocate(&mnt, f, 0x08, 0, 2 * PAGE_SIZE) == -EOPNOTSUPP && fsize_signals == 2);
    CHECK(tmpfs_fallocate(&mnt, f, 0x03, 0, 2 * PAGE_SIZE) == 0 && fsize_signals == 2);
    CHECK(tmpfs_fallocate(&mnt, f, 0, 0, PAGE_SIZE) == 0 && n->data.size == PAGE_SIZE &&
          frames_in_use() == frames + 1);
    fsize_limit = ~(uint64_t)0;
    CHECK(tmpfs_unlink(&mnt, NULL, "fl") == 0);
    tmpfs_put(&mnt, f);
    CHECK(frames_in_use() == frames);
}

/** Make the file `name` in the root with one page in each of leaves 0 to
 *  3, so that its table is the four leaves, each present. Returns it,
 *  held. */
static void *four_leaves(const char *name)
{
    void *f = make(NULL, name, 0644);
    uint32_t leaf;

    for (leaf = 0; leaf < 4; leaf++) {
        CHECK(tmpfs_write(&mnt, f, "x", 1, (uint64_t)leaf * SHMEM_LEAF * PAGE_SIZE) == 1);
    }
    CHECK(((struct tmpfs_node *)f)->data.nleaves == 4);
    return f;
}

/* The node a rename moves, and the give-ways that found it by its new
 * name. */
static void *moved;
static unsigned moved_found;

/** At a give-way of the replaced file's free: whether the moved node has
 *  its new name, "dst", already. */
static void find_moved(void)
{
    void *n = look(NULL, "dst");

    moved_found += n != NULL && n == moved;
    if (n) {
        tmpfs_put(&mnt, n);
    }
}

/**
 * The give-ways of a file's free. A file with a page in each of four
 * leaves gives no way at an unlink while a hold remains, and three, one
 * between each two leaves, at its last put. A rename over such a file
 * gives way three times as it frees it, and at each the moved file is
 * found by its new name.
 */
static void give_way_per_leaf(void)
{
    int frames = frames_in_use();
    void *f = four_leaves("big");

    CHECK(frames_in_use() == frames + 4);
    give_ways = 0;
    CHECK(tmpfs_unlink(&mnt, NULL, "big") == 0 && give_ways == 0);
    tmpfs_put(&mnt, f);
    CHECK(give_ways == 3 && frames_in_use() == frames);

    tmpfs_put(&mnt, four_leaves("dst"));
    moved = make(NULL, "src", 0644);
    tmpfs_put(&mnt, moved);
    give_ways = 0;
    moved_found = 0;
    at_give_way = find_moved;
    CHECK(tmpfs_rename(&mnt, NULL, "src", NULL, "dst") == 0);
    at_give_way = NULL;
    CHECK(give_ways == 3 && moved_found == 3 && frames_in_use() == frames);
    CHECK(look(NULL, "src") == NULL);
    CHECK(tmpfs_unlink(&mnt, NULL, "dst") == 0);
}

/** Put `n` when it is not NULL, and say whether it was `want`. */
static int put_is(void *n, void *want)
{
    if (n) {
        tmpfs_put(&mnt, n);
    }
    return n == want;
}

/**
 * renameat2's RENAME_EXCHANGE. A file "f" in "ex1" and a directory "s" in
 * "ex2" swap: each name keeps its place in its listing, the directory's
 * parent and both directories' links follow it, and its path is the new
 * one. A swap that would put "ex1" below itself is EINVAL, one with a name
 * missing is ENOENT, and one of two names of one file changes nothing.
 */
static void rename_exchange(void)
{
    struct vfs_dirent ent;
    struct vfs_stat st;
    char path[32];
    uint32_t cursor = 2;
    void *d1, *d2, *f, *sub;

    CHECK(tmpfs_mkdir(&mnt, NULL, "ex1", 0755) == 0);
    CHECK(tmpfs_mkdir(&mnt, NULL, "ex2", 0755) == 0);
    d1 = look(NULL, "ex1");
    d2 = look(NULL, "ex2");
    f = make(d1, "f", 0644);
    CHECK(tmpfs_mkdir(&mnt, d1, "g", 0755) == 0);
    CHECK(tmpfs_mkdir(&mnt, d2, "s", 0755) == 0);
    sub = look(d2, "s");
    CHECK(tmpfs_rename_flags(&mnt, d1, "f", d2, "s", VFS_RENAME_EXCHANGE) == 0);
    CHECK(put_is(look(d1, "f"), sub) && put_is(look(d2, "s"), f));
    CHECK(tmpfs_getattr(&mnt, d1, &st) == 0 && st.nlink == 4);
    CHECK(tmpfs_getattr(&mnt, d2, &st) == 0 && st.nlink == 2);
    CHECK(tmpfs_path(&mnt, sub, path, sizeof(path)) == 0 && same(path, "/ex1/f"));
    CHECK(tmpfs_readdir(&mnt, d1, &cursor, &ent) == 1 && same(ent.name, "f") && ent.is_dir);
    CHECK(tmpfs_readdir(&mnt, d1, &cursor, &ent) == 1 && same(ent.name, "g"));
    CHECK(tmpfs_rename_flags(&mnt, NULL, "ex1", d1, "f", VFS_RENAME_EXCHANGE) == -EINVAL);
    CHECK(tmpfs_rename_flags(&mnt, d1, "f", NULL, "ex1", VFS_RENAME_EXCHANGE) == -EINVAL);
    CHECK(tmpfs_rename_flags(&mnt, d1, "none", d2, "s", VFS_RENAME_EXCHANGE) == -ENOENT);
    CHECK(tmpfs_rename_flags(&mnt, d1, "f", d2, "none", VFS_RENAME_EXCHANGE) == -ENOENT);
    CHECK(tmpfs_link(&mnt, d2, "h", f) == 0);
    CHECK(tmpfs_rename_flags(&mnt, d2, "s", d2, "h", VFS_RENAME_EXCHANGE) == 0);
    CHECK(put_is(look(d2, "s"), f) && put_is(look(d2, "h"), f));
    CHECK(tmpfs_rename_flags(&mnt, d1, "f", d2, "s", VFS_RENAME_EXCHANGE | VFS_RENAME_NOREPLACE) ==
          -EINVAL);
    tmpfs_put(&mnt, sub);
    tmpfs_put(&mnt, f);
    tmpfs_put(&mnt, d2);
    tmpfs_put(&mnt, d1);
}

/**
 * renameat2's RENAME_WHITEOUT. "wf" moves to "wg", and "wf" is then a
 * character device 0:0 of mode 0 with one link. A rename the heap cannot
 * serve, at each of its allocations, is ENOSPC with both names as they
 * were. A rename of a file over a directory is EISDIR, and of a directory
 * over one that is not empty ENOTEMPTY, each with no whiteout made.
 */
static void rename_whiteout(void)
{
    struct vfs_stat st;
    void *d, *w, *white, *full;
    int n;

    CHECK(tmpfs_mkdir(&mnt, NULL, "wo", 0755) == 0);
    d = look(NULL, "wo");
    w = make(d, "wf", 0640);
    CHECK(tmpfs_rename_flags(&mnt, d, "wf", d, "wg", VFS_RENAME_WHITEOUT) == 0);
    CHECK(put_is(look(d, "wg"), w));
    white = look(d, "wf");
    CHECK(white != NULL && tmpfs_getattr(&mnt, white, &st) == 0 && st.mode == T_IFCHR &&
          st.rdev == 0 && st.nlink == 1);
    for (n = 1; n <= 5; n++) {
        heap_fails_in = n;
        CHECK(tmpfs_rename_flags(&mnt, d, "wg", d, "wh", VFS_RENAME_WHITEOUT) == -ENOSPC);
        heap_fails_in = 0;
        CHECK(put_is(look(d, "wg"), w) && put_is(look(d, "wh"), NULL));
    }
    CHECK(tmpfs_mkdir(&mnt, d, "full", 0755) == 0);
    full = look(d, "full");
    CHECK(tmpfs_mkdir(&mnt, full, "in", 0755) == 0);
    CHECK(tmpfs_rename_flags(&mnt, d, "wg", d, "full",
                             VFS_RENAME_WHITEOUT | VFS_RENAME_NOREPLACE) == -EISDIR);
    CHECK(tmpfs_mkdir(&mnt, d, "other", 0755) == 0);
    CHECK(tmpfs_rename_flags(&mnt, d, "other", d, "full", VFS_RENAME_WHITEOUT) == -ENOTEMPTY);
    CHECK(put_is(look(d, "wg"), w) && put_is(look(d, "wf"), white));
    tmpfs_put(&mnt, full);
    tmpfs_put(&mnt, white);
    tmpfs_put(&mnt, w);
    tmpfs_put(&mnt, d);
}

/**
 * Hard links to files with no name. A file whose last name went takes no
 * new one, ENOENT, as on Linux. An O_TMPFILE file has no name and no link
 * from the start; one made linkable takes one link, and after that link is
 * removed it takes no other; one made without is ENOENT at once. A
 * tmpfile in a removed directory is ENOENT.
 */
static void unnamed_links(void)
{
    struct vfs_stat st;
    void *d, *q, *t = NULL;

    CHECK(tmpfs_mkdir(&mnt, NULL, "tf", 0755) == 0);
    d = look(NULL, "tf");
    q = make(d, "q", 0644);
    CHECK(tmpfs_unlink(&mnt, d, "q") == 0);
    CHECK(tmpfs_link(&mnt, d, "q2", q) == -ENOENT);
    tmpfs_put(&mnt, q);
    CHECK(tmpfs_tmpfile(&mnt, d, 0600, 1, &t) == 0);
    CHECK(tmpfs_getattr(&mnt, t, &st) == 0 && st.mode == (T_IFREG | 0600u) && st.nlink == 0);
    CHECK(tmpfs_link(&mnt, d, "t", t) == 0);
    CHECK(tmpfs_getattr(&mnt, t, &st) == 0 && st.nlink == 1);
    CHECK(tmpfs_unlink(&mnt, d, "t") == 0);
    CHECK(tmpfs_link(&mnt, d, "t2", t) == -ENOENT);
    tmpfs_put(&mnt, t);
    CHECK(tmpfs_tmpfile(&mnt, d, 0600, 0, &t) == 0);
    CHECK(tmpfs_link(&mnt, d, "t3", t) == -ENOENT);
    tmpfs_put(&mnt, t);
    CHECK(tmpfs_rmdir(&mnt, NULL, "tf") == 0);
    CHECK(tmpfs_tmpfile(&mnt, d, 0600, 1, &t) == -ENOENT);
    tmpfs_put(&mnt, d);
}

/**
 * Mount with mode= and size= and check the root's mode and links, mount
 * again with no options, whose root is 01777, and run the rows on that
 * mount. Returns the count of failed checks, at most 100.
 */
int main(void)
{
    struct vfs_stat st;

    CHECK(tmpfs_mount(&mnt, NULL, 0, "size=1m,mode=0755") == 0);
    CHECK(tmpfs_getattr(&mnt, mnt.root, &st) == 0 && st.mode == (T_IFDIR | 0755u) && st.nlink == 2);
    tmpfs_unmount(&mnt);
    CHECK(tmpfs_mount(&mnt, NULL, 0, "") == 0);
    CHECK(tmpfs_getattr(&mnt, mnt.root, &st) == 0 && st.mode == (T_IFDIR | 01777u));
    bytes();
    sparse();
    directories();
    links_and_renames();
    kinds();
    listing();
    listing_removal();
    mapping();
    truncation();
    seals();
    removed_parent();
    removed_directory();
    rename_refused();
    sizes();
    flags();
    attributes();
    inodes();
    limit_options();
    allocation();
    give_way_per_leaf();
    rename_exchange();
    rename_whiteout();
    unnamed_links();
    tmpfs_unmount(&mnt);
    CHECK(frames_in_use() == 0);
    return fails > 100 ? 100 : fails;
}
