/**
 * Check the two counts of an address space: the users, whose last put frees
 * the memory, and the holds, which keep only the record; the program's
 * file it keeps; and a thread's holds of its regions' lock, which
 * `--lock-only` runs alone.
 *
 * host-deps: tests/host/compat/vm_iter_host.c rach2/sys/mm/notify.c
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-deps: tests/host/compat/spinlock_host.c tests/host/compat/atomic_host.c
 *
 * `mm/mm.c` is included whole, over a model of the paging layer that counts
 * what is freed. A hold, which a /proc/PID/mem descriptor takes, must keep
 * the record and nothing else: the last user's put frees the pages, the
 * directory and the regions while the hold remains, a holder then takes no
 * user's count, and the last hold frees the record, which AddressSanitizer
 * judges by any use after.
 *
 * The file system's part of the program's file is a model that makes a
 * description with a count and, at each release, notes whether the holder
 * count still finds the file: an address space must be off the list before
 * its release, so a release sees only the others that keep the file. It
 * notes the same when the release's lock is taken, which must come while the
 * address space is still on the list, so that no count runs between its
 * leaving and the release.
 */
#include "mm/mm.c"
#include "arch/i686/mmu.h"
#include "fs/vfs.h"

extern void *calloc(unsigned long count, unsigned long size);
extern void free(void *ptr);
extern int printf(const char *format, ...);
extern void exit(int status);

page_dir_t *_kernel_dir;
struct percpu cpu_table[NR_CPUS];
static page_dir_t kernel_dir;
/* The directories made, the frees of each part, the reaps, and the rows run. */
static int dirs, pages_freed, dirs_freed, regions_freed, reaps, rows, failures;
/* The mount and the nodes of the program's file rows, the descriptions made
 * and released, and whether the holder count found the file at each
 * release. */
static struct vfs_mount root;
static int program_node, other_node;
static int exe_opens, exe_closes, refuse_exe;
static int held_at_close[4];
static int exe_locks, held_at_lock[4];

/** The file system a bind mount shows, or the mount itself. */
const struct vfs_mount *vfs_mount_base(const struct vfs_mount *m)
{
    return m->bind_of ? m->bind_of : m;
}

/** Count and print a failure when `c` is false. */
#define CHECK(c, detail)                           \
    do {                                           \
        if (!(c)) {                                \
            printf("mm-test: fail: %s\n", detail); \
            failures++;                            \
        }                                          \
    } while (0)

/** The resident count of the page tables, which this model has none of: 0
 * pages present, dirty or anonymous. */
void paging_user_count(page_dir_t *dir, uintptr_t start, uintptr_t end, unsigned long *present,
                       unsigned long *dirty, unsigned long *anon)
{
    (void)dir;
    (void)start;
    (void)end;
    *present = *dirty = *anon = 0;
}

/** The frames of the page tables, which this model has none of. */
void paging_user_frames(page_dir_t *dir, uintptr_t start, uintptr_t end,
                        struct paging_frame_counts *counts)
{
    (void)dir;
    (void)start;
    (void)end;
    k_memset(counts, 0, sizeof(*counts));
}

/** The kernel's allocator, from the C library, zeroed. */
void *kmalloc(size_t size)
{
    return calloc(1, size);
}

/** The kernel's zeroed allocator, from the C library. */
void *kzalloc(size_t size)
{
    return calloc(1, size);
}

/** Free a block of the allocators above. */
void kfree(void *p)
{
    free(p);
}

/** A new directory, counted. */
page_dir_t *clone_page_directory(void)
{
    dirs++;
    return calloc(1, sizeof(page_dir_t));
}

/** Every directory's physical address: 0x1000. */
uintptr_t get_page_dir_phys(page_dir_t *dir)
{
    (void)dir;
    return 0x1000u;
}

/** 0: the host has no interrupt flag. */
uint32_t kirq_save(void)
{
    return 0;
}

/** Nothing: the host has no interrupt flag. */
void kirq_restore(uint32_t flags)
{
    (void)flags;
}

/* The rwsem calls the regions' lock makes: a thread's first hold takes it
 * and its last gives it back, and the nested ones only count. */
static int rw_downs, rw_ups, rw_trylock_refuse;

/** Count the rwsem's making. */
void rwsem_init_key(struct rwsem *s, const char *name, const struct lockdep_key *key)
{
    (void)s;
    (void)name;
    (void)key;
}

/** Count a read take. */
void down_read(struct rwsem *s)
{
    (void)s;
    rw_downs++;
}

/** Count a read take that needs no wait. */
int down_read_trylock(struct rwsem *s)
{
    (void)s;
    rw_downs++;
    return 1;
}

/** Count a write take. */
void down_write(struct rwsem *s)
{
    (void)s;
    rw_downs++;
}

/** Count a write take, or refuse it when the row says another holds it. */
int down_write_trylock(struct rwsem *s)
{
    (void)s;
    if (rw_trylock_refuse) {
        return 0;
    }
    rw_downs++;
    return 1;
}

/** Count a read release. */
void up_read(struct rwsem *s)
{
    (void)s;
    rw_ups++;
}

/** Count a write release. */
void up_write(struct rwsem *s)
{
    (void)s;
    rw_ups++;
}

/** 1: the test is the one thread, under the big lock. */
int bkl_held(void)
{
    return 1;
}

/** Nothing: the test runs under the big lock throughout. */
void bkl_enter(void)
{}

/** Nothing, as `bkl_enter`. */
void bkl_exit(void)
{}

/** 1: a file may wait, so that `mm_reap` reaches `vm_file_reap`. */
int vm_file_dead(void)
{
    return 1;
}

/** Stop the test: a panic is a failure of the row that made it. */
void k_panic(const char *str)
{
    printf("mm-test: panic: %s\n", str);
    exit(1);
}

/** Load `dir`. */
void switch_page_directory(page_dir_t *dir)
{
    _curr_dir = dir;
}

/** The other processors give `dir`, which must not be NULL, back at once. */
void smp_space_release(const page_dir_t *dir)
{
    CHECK(dir != NULL, "the other processors are asked to give the directory back");
}

/** The architecture keeps nothing with an address space here. */
void arch_mm_release(struct mm *mm)
{
    (void)mm;
}

/** Count a free of the pages. */
void free_user_pages(page_dir_t *dir)
{
    CHECK(dir != NULL, "the pages are freed through the directory");
    pages_freed++;
}

/** Count and free a directory, which must not be the one loaded. */
void free_page_directory(page_dir_t *dir)
{
    CHECK(dir != NULL && dir != _curr_dir, "the directory freed is not the one in use");
    dirs_freed++;
    free(dir);
}

/** Count a free of the regions. */
void vm_free(vm_space_t *vm)
{
    (void)vm;
    regions_freed++;
}

/** Count a reap of the mapped files. */
void vm_file_reap(void)
{
    reaps++;
}

/** A description of the program's file with one reference; NULL when the
 * row refuses it. */
struct open_file *fs_exe_open(const struct vfs_ref *ref)
{
    struct open_file *ofd;

    if (refuse_exe) {
        return NULL;
    }
    ofd = calloc(1, sizeof(*ofd));
    if (ofd) {
        ofd->refcount = 1;
        ofd->vmnt = ref->mnt;
        ofd->vnode = ref->node;
        exe_opens++;
    }
    return ofd;
}

/** Take the release's lock, noting whether the holder count finds the file
 * then; the mount locked. */
struct vfs_mount *fs_exe_lock(const struct open_file *ofd)
{
    held_at_lock[exe_locks++ & 3] = mm_exe_holds(ofd->vmnt, ofd->vnode);
    return &root;
}

/** Note whether the holder count finds the file still, and drop the
 * reference; the lock the release took comes with it. */
void fs_exe_close(struct open_file *ofd, struct vfs_mount *locked)
{
    CHECK(locked == &root && exe_locks == exe_closes + 1, "a release comes with its lock");
    held_at_close[exe_closes++ & 3] = mm_exe_holds(ofd->vmnt, ofd->vnode);
    if (--ofd->refcount == 0) {
        free(ofd);
    }
}

/** No row models writers; executable reservations always succeed. */
int fd_ofd_access(struct open_file *ofd, const struct vfs_ref *ref, int access)
{
    (void)ofd;
    (void)ref;
    (void)access;
    return 0;
}

/** Take a reference to the description `ofd`. */
void fd_ofd_ref(struct open_file *ofd)
{
    ofd->refcount++;
}

/** A /proc/PID/mem descriptor's hold keeps the record and none of the
 * memory: the last user's put frees the pages, the directory and the
 * regions at once, the holder then takes no user's count, and its unhold
 * frees the record. */
static void hold_keeps_record(void)
{
    struct mm *mm = mm_new();

    CHECK(mm && mm->users == 1 && mm->holds == 1, "a new address space has a user and a hold");
    if (!mm) {
        return;
    }
    mm_set_exe(mm, "/bin/prog");
    mm_hold(mm);
    mm_put(mm);
    CHECK(pages_freed == 1 && dirs_freed == 1 && regions_freed == 1 && reaps == 1,
          "the last user's put frees the memory while a descriptor holds the record");
    CHECK(mm->dir == NULL && mm->holds == 1 && mm->exe != NULL,
          "and the record stays for the holder, its directory gone");
    CHECK(mm_get_live(mm) == 0 && mm->users == 0,
          "a holder takes no user's count once none is left");
    mm_unhold(mm);
    rows++;
}

/** A holder's user's count while a user remains frees nothing at its put,
 * and the last user's put then frees the memory. */
static void live_user(void)
{
    struct mm *mm = mm_new();

    if (!mm) {
        CHECK(0, "a new address space");
        return;
    }
    pages_freed = dirs_freed = regions_freed = 0;
    mm_hold(mm);
    CHECK(mm_get_live(mm) == 1 && mm->users == 2,
          "a holder takes a user's count while a user remains");
    mm_put(mm);
    CHECK(pages_freed == 0 && dirs_freed == 0 && regions_freed == 0 && mm->dir != NULL,
          "whose put, not the last, frees nothing");
    mm_put(mm);
    CHECK(pages_freed == 1 && dirs_freed == 1 && regions_freed != 0,
          "and the last user's put frees the memory");
    mm_unhold(mm);
    rows++;
}

/** A thread's end loads the kernel's directory first only when it is the
 * last user of the directory in use. */
static void drop_switches(void)
{
    process_t a, b;
    struct mm *mm = mm_new();

    if (!mm) {
        CHECK(0, "a new address space");
        return;
    }
    k_memset(&a, 0, sizeof(a));
    k_memset(&b, 0, sizeof(b));
    a.mm = b.mm = mm;
    mm_get(mm);
    mm_hold(mm);
    _curr_dir = mm->dir;
    mm_drop(&a);
    CHECK(a.mm == NULL && _curr_dir == mm->dir && mm->users == 1,
          "a thread that is not the last user leaves the directory in use");
    mm_drop(&b);
    CHECK(_curr_dir == _kernel_dir && mm->dir == NULL,
          "the last user's end switches to the kernel's directory before it frees");
    mm_unhold(mm);
    rows++;
}

/**
 * An address space keeps its program's file on the list the holder count
 * reads; a fork's copy shares the description and is on it too; the last
 * user's put of each takes the release's lock while it is still on the list,
 * then leaves the list before the release, so the first release finds the
 * file still held by the other and the last finds it held no more, and a
 * hold on the record, as a `/proc/PID/mem` descriptor has, keeps no file past
 * that put; and one whose description the heap has no room for keeps none.
 */
static void program_file(void)
{
    struct vfs_ref ref;
    struct mm *parent = mm_new();
    struct mm *child = mm_new();

    if (!parent || !child) {
        CHECK(0, "two new address spaces");
        return;
    }
    ref.mnt = &root;
    ref.node = &program_node;
    CHECK(mm_exe_open(parent, &ref) == 0 && exe_opens == 1,
          "an address space keeps its program's file");
    CHECK(mm_exe_holds(&root, &program_node) && !mm_exe_holds(&root, &other_node) &&
              !mm_exe_holds(NULL, &program_node),
          "the holder count finds the file's node on its mount");
    mm_copy_info(child, parent);
    CHECK(child->exe_file == parent->exe_file && parent->exe_file->refcount == 2,
          "a fork's copy shares the description");
    mm_hold(parent);
    mm_put(parent);
    CHECK(exe_closes == 1 && held_at_close[0] == 1 && mm_exe_holds(&root, &program_node),
          "the first end leaves the file held by the copy, with a hold on its record left");
    mm_unhold(parent);
    mm_put(child);
    CHECK(exe_locks == 2 && held_at_lock[1] == 1,
          "the last end takes the release's lock while it still holds the file");
    CHECK(exe_closes == 2 && held_at_close[1] == 0 && !mm_exe_holds(&root, &program_node),
          "the last end releases the file off the list");
    refuse_exe = 1;
    parent = mm_new();
    CHECK(parent && mm_exe_open(parent, &ref) == -ENOMEM && parent->exe_file == NULL &&
              !mm_exe_holds(&root, &program_node),
          "a description the heap has no room for keeps no file");
    refuse_exe = 0;
    mm_put(parent);
    rows++;
}

/* The thread whose holds of the regions' locks `lock_holds` counts. */
static process_t lock_thread;

/** A thread's holds of the regions' locks: a hold taken again of either
 * kind only counts, and the rwsem is taken by the first and given back by
 * the last; one read hold alone may be let go around a sleep; a write
 * wanted by a reader is refused, since it would wait for itself; a second
 * address space has a record of its own; and the files are reaped only
 * once the thread holds no regions' lock. */
static void lock_holds(void)
{
    struct mm *a = mm_new();
    struct mm *b = mm_new();
    int before;

    if (!a || !b) {
        CHECK(0, "two address spaces");
        return;
    }
    cpu_table[0].current = &lock_thread;
    rw_downs = rw_ups = 0;
    mmap_read_lock(a);
    mmap_read_lock(a);
    CHECK(rw_downs == 1 && mmap_held(a, 0) && !mmap_held(a, 1) && !mmap_may_drop(a),
          "a read hold taken again only counts, and may not be let go");
    mmap_unlock(a);
    CHECK(rw_ups == 0 && mmap_may_drop(a),
          "the inner release gives nothing back, and the one hold left may be let go");
    CHECK(mmap_write_trylock(a) == 0 && rw_downs == 1 && !mmap_held(a, 1),
          "a write hold is refused to a reader of the same space");
    mmap_write_lock(b);
    mmap_read_lock(b);
    CHECK(rw_downs == 2 && mmap_held(b, 1) && !mmap_may_drop(b),
          "a writer's read takes nothing more, in a record of the second space's own");
    before = reaps;
    mm_reap();
    CHECK(reaps == before, "no file is reaped while the thread holds a regions' lock");
    mmap_unlock(b);
    mmap_unlock(b);
    mmap_unlock(a);
    CHECK(rw_ups == 2 && !mmap_held(a, 0) && !mmap_held(b, 0),
          "the last release of each space gives its rwsem back");
    mm_reap();
    CHECK(reaps == before + 1, "with no hold left the files are reaped");
    rw_trylock_refuse = 1;
    CHECK(mmap_write_trylock(a) == 0 && !mmap_held(a, 0),
          "a write trylock that finds the lock taken holds nothing");
    rw_trylock_refuse = 0;
    cpu_table[0].current = NULL;
    mm_put(a);
    mm_put(b);
    rows++;
}

/** Run the rows, or with `--lock-only` the holds of the regions' locks
 * alone; 0 when every check passed, 1 otherwise. */
int main(int argc, char **argv)
{
    _kernel_dir = &kernel_dir;
    _curr_dir = _kernel_dir;
    if (argc == 2 && __builtin_strcmp(argv[1], "--lock-only") == 0) {
        lock_holds();
        return failures ? 1 : 0;
    }
    hold_keeps_record();
    live_user();
    drop_switches();
    program_file();
    lock_holds();
    CHECK(rows == 5 && dirs == 8, "every row ran");
    if (failures) {
        return 1;
    }
    printf("mm-test: pass: a hold keeps the record, a user the memory and the program's file, "
           "and a thread's lock holds count\n");
    return 0;
}
