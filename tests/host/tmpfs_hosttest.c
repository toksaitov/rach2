/**
 * Host test for tmpfs and the in-memory file bytes under it (fs/tmpfs.c,
 * mm/shmem.c).
 *
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 *
 * Includes both sources and stands a pool of host pages in for the frames.
 * A file's bytes are written across a hole and a page boundary, read back with
 * the hole as zeros, and cut and grown again; frames come filled with 0xA5, so
 * a zero read is one the code wrote. A file whose first pages are a hole reads
 * a whole page of it as zeros into a buffer that held other bytes, and a write
 * and a read across a page boundary move every byte. The mount option sets the
 * root's mode. Directories count their subdirectories in their link count and
 * refuse rmdir while they have names; a hard link keeps the bytes after the
 * first name goes; rename moves a name, replaces a file, and refuses to put a
 * directory over a file, a file over a directory, a directory over one that is
 * not empty, or a directory under itself, and one the heap cannot serve changes
 * nothing. A symbolic link reads back its target, a FIFO keeps its type, a
 * device node keeps its number and does not open, a listing is ".", "..", then
 * the names in the order they were made or renamed, each with its type, and
 * goes on past a name removed during it, and a directory knows its path. A node
 * held after its last name goes stays readable, a removed directory held after
 * its parent is removed keeps the parent for its "..", and a shared mapping's
 * page takes a reference of its own; a page at or past the most a file may have
 * is refused without growing the table, 2^31 and 2^32 - 1 among them, and the
 * last page below it is given; a truncation of mapped memory takes the pages it
 * frees out of the mappings first. A memfd's node has its name and no
 * directory, and goes with its last hold; its seals refuse a shrink, a growth,
 * a write, a change of its execute bits and more seals, and a named file starts
 * sealed against sealing. Returns the failed-check count.
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
process_t *current_proc = &process;

/* When positive, the allocation that counts it down to zero is refused. */
static int heap_fails_in;

static int heap_refuses(void)
{
    return heap_fails_in > 0 && --heap_fails_in == 0;
}

void *kmalloc(size_t n)
{
    return heap_refuses() ? NULL : malloc(n);
}

void *kzalloc(size_t n)
{
    return heap_refuses() ? NULL : calloc(1, n);
}

void kfree(void *p)
{
    free(p);
}

void get_current_time(int64_t *seconds, int64_t *nanoseconds)
{
    *seconds = 1000;
    *nanoseconds = 0;
}

#define POOL 64
static unsigned char pool[POOL][PAGE_SIZE];
static int refs[POOL];

static int frame_index(uintptr_t phys)
{
    return (int)(phys / PAGE_SIZE) - 1;
}

/* A frame comes filled with 0xA5, as a real one comes with what its last
 * user left, so a page that reads as zeros was zeroed by the code under
 * test. */
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

void free_frame(uintptr_t phys)
{
    refs[frame_index(phys)]--;
}

void frame_ref(uintptr_t phys)
{
    refs[frame_index(phys)]++;
}

void *kmap_temp(uintptr_t phys)
{
    return pool[frame_index(phys)];
}

void kunmap_temp(void *va)
{
    (void)va;
}

int paging_zero_frame(uintptr_t phys)
{
    k_memset(pool[frame_index(phys)], 0, PAGE_SIZE);
    return 0;
}

uint32_t frame_total_count(void)
{
    return POOL;
}

uint32_t frame_free_count(void)
{
    uint32_t n = 0;
    int i;

    for (i = 0; i < POOL; i++) {
        n += refs[i] == 0;
    }
    return n;
}

static int frames_in_use(void)
{
    return POOL - (int)frame_free_count();
}

/* The mapping side's part, which a truncation of mapped memory calls before
 * it frees the pages: counted, with the first page it was given, and every
 * mapped page past it let go, as the regions' page tables would let go. */
static int cuts;
static uint32_t cut_first;
static uintptr_t mapped[4];

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

static void *make(void *dir, const char *name, uint32_t mode)
{
    void *n = NULL;

    CHECK(tmpfs_create(&mnt, dir, name, mode, &n) == 0);
    return n;
}

static void *look(void *dir, const char *name)
{
    void *n = NULL;

    return tmpfs_lookup(&mnt, dir, name, &n) == 0 ? n : NULL;
}

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

static int same(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

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
    CHECK(frames_in_use() == 2); /* pages 0 and 1; the hole between is none */
    CHECK(tmpfs_read(&mnt, f, buf, 5, 0) == 5 && k_memcmp(buf, "hello", 5) == 0);
    CHECK(tmpfs_read(&mnt, f, buf, 4, 8188) == 4 && buf[0] == 0 && buf[1] == 0 && buf[2] == 'x');
    CHECK(tmpfs_read(&mnt, f, buf, 4, 8192) == 0);
    CHECK(tmpfs_truncate(&mnt, f, 3) == 0 && frames_in_use() == 1);
    CHECK(tmpfs_truncate(&mnt, f, 10) == 0);
    CHECK(tmpfs_read(&mnt, f, buf, 10, 0) == 10 && k_memcmp(buf, "hel\0\0\0\0\0\0\0", 10) == 0);
    tmpfs_put(&mnt, f);
}

/* Whether the `n` bytes at `p` are all zero. */
static int all_zero(const char *p, size_t n)
{
    while (n-- > 0) {
        if (*p++ != 0) {
            return 0;
        }
    }
    return 1;
}

/* A file whose first pages are a hole reads them as zeros into a buffer that
 * held something else, and a write and a read across a page boundary move
 * every byte: the write takes two frames, and the read returns the bytes of
 * both pages and the hole's zeros after them. */
static void sparse(void)
{
    static char buf[PAGE_SIZE + 16];
    void *f = make(NULL, "s", 0644);
    int frames = frames_in_use();

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
}

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
    }
    {
        void *r = make(NULL, "r", 0644);

        CHECK(tmpfs_write(&mnt, r, "new", 3, 0) == 3);
        tmpfs_put(&mnt, r);
        CHECK(tmpfs_rename(&mnt, NULL, "r", NULL, "c") == 0);
        CHECK(look(NULL, "r") == NULL);
    }
    /* "c" was replaced, but its node is held and still reads. */
    CHECK(tmpfs_read(&mnt, held, buf, 3, 0) == 3 && k_memcmp(buf, "hel", 3) == 0);
    CHECK(tmpfs_getattr(&mnt, held, &st) == 0 && st.nlink == 0);
    tmpfs_put(&mnt, held);
    tmpfs_put(&mnt, y);
    tmpfs_put(&mnt, x);
}

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
    CHECK(shmem_map_page(sh, 0xFFFFFFFFu) == 0 && shmem_map_page(sh, 0x80000000u) == 0 &&
          shmem_map_page(sh, most) == 0 && sh->npages == npages);
    last = shmem_map_page(sh, most - 1u);
    CHECK(last != 0 && sh->npages == most && refs[frame_index(last)] == 2);
    CHECK(tmpfs_truncate(&mnt, f, 0) == 0 && refs[frame_index(phys)] == 1 &&
          refs[frame_index(last)] == 1);
    free_frame(phys);
    free_frame(last);
    tmpfs_put(&mnt, f);
}

/* A truncation of memory a mapping maps takes the pages it frees out of the
 * mapping first, from the first page past the new end, so that after the
 * file grows again a write and the mapping's next touch meet in one page.
 * Memory no mapping maps is truncated without the mapping side. */
static void truncation(void)
{
    void *f = make(NULL, "t", 0644);
    struct shmem *sh = tmpfs_shmem(f);

    CHECK(sh != NULL && tmpfs_write(&mnt, f, "abcd", 4, 3 * PAGE_SIZE) == 4);
    sh->maps = 1;
    mapped[0] = shmem_map_page(sh, 0);
    mapped[3] = shmem_map_page(sh, 3);
    cuts = 0;
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

    /* Sealed against a change of its execute bits, a file that has them is
     * sealed against writing too. */
    CHECK(tmpfs_memfd(&mnt, "exec", 0755, 0, &f) == 0 && tmpfs_add_seals(f, TMPFS_SEAL_EXEC) == 0 &&
          (tmpfs_seals(f) & TMPFS_SEAL_WRITE));
    st.mode = 0644;
    CHECK(tmpfs_setattr(&mnt, f, VFS_SET_MODE, &st) == -EPERM);
    st.mode = 0755 | 04000;
    CHECK(tmpfs_setattr(&mnt, f, VFS_SET_MODE, &st) == 0);
    tmpfs_put(&mnt, f);
}

/**
 * A removed directory that is still held keeps its removed parent: its ".."
 * is listed with the parent's number and walked to the parent, and both go
 * with the last hold.
 */
static void removed_parent(void)
{
    struct vfs_dirent ent;
    struct vfs_stat st;
    uint32_t cursor = 0;
    void *p, *c;

    CHECK(tmpfs_mkdir(&mnt, NULL, "rp", 0755) == 0);
    p = look(NULL, "rp");
    CHECK(tmpfs_getattr(&mnt, p, &st) == 0 && tmpfs_mkdir(&mnt, p, "rc", 0755) == 0);
    c = look(p, "rc");
    tmpfs_put(&mnt, p);
    CHECK(tmpfs_rmdir(&mnt, p, "rc") == 0 && tmpfs_rmdir(&mnt, NULL, "rp") == 0);
    CHECK(tmpfs_readdir(&mnt, c, &cursor, &ent) == 1 &&
          tmpfs_readdir(&mnt, c, &cursor, &ent) == 1 && same(ent.name, "..") && ent.ino == st.ino);
    CHECK(look(c, "..") == p);
    tmpfs_put(&mnt, p);
    tmpfs_put(&mnt, c);
}

/**
 * A rename the heap cannot serve changes nothing, whether its entry or its
 * name is refused: a directory with a file in it keeps its old name and the
 * file, and the new name is not made.
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
    rename_refused();
    tmpfs_unmount(&mnt);
    CHECK(frames_in_use() == 0);
    return fails > 100 ? 100 : fails;
}
