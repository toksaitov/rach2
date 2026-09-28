/**
 * Check the two counts of an address space: the users, whose last put frees
 * the memory, and the holds, which keep only the record; and the program's
 * file it keeps.
 *
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
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
#include "fs/vfs.h"

extern void *calloc(unsigned long count, unsigned long size);
extern void free(void *ptr);
extern int printf(const char *format, ...);

page_dir_t *_kernel_dir;
page_dir_t *_curr_dir;
static page_dir_t kernel_dir;
static int dirs, pages_freed, dirs_freed, regions_freed, reaps, rows, failures;
/* The mount and the nodes of the program's file rows, the descriptions made
 * and released, and whether the holder count found the file at each
 * release. */
static struct vfs_mount root;
static int program_node, other_node;
static int exe_opens, exe_closes, refuse_exe;
static int held_at_close[4];
static int exe_locks, held_at_lock[4];

#define CHECK(c, detail)                           \
    do {                                           \
        if (!(c)) {                                \
            printf("mm-test: fail: %s\n", detail); \
            failures++;                            \
        }                                          \
    } while (0)

void *kmalloc(size_t size)
{
    return calloc(1, size);
}

void *kzalloc(size_t size)
{
    return calloc(1, size);
}

void kfree(void *p)
{
    free(p);
}

page_dir_t *clone_page_directory(void)
{
    dirs++;
    return calloc(1, sizeof(page_dir_t));
}

uintptr_t get_page_dir_phys(page_dir_t *dir)
{
    (void)dir;
    return 0x1000u;
}

void switch_page_directory(page_dir_t *dir)
{
    _curr_dir = dir;
}

void free_user_pages(page_dir_t *dir)
{
    CHECK(dir != NULL, "the pages are freed through the directory");
    pages_freed++;
}

void free_page_directory(page_dir_t *dir)
{
    CHECK(dir != NULL && dir != _curr_dir, "the directory freed is not the one in use");
    dirs_freed++;
    free(dir);
}

void vm_free(vm_space_t *vm)
{
    (void)vm;
    regions_freed++;
}

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

int main(void)
{
    _kernel_dir = &kernel_dir;
    _curr_dir = _kernel_dir;
    hold_keeps_record();
    live_user();
    drop_switches();
    program_file();
    CHECK(rows == 4 && dirs == 6, "every row ran");
    if (failures) {
        return 1;
    }
    printf("mm-test: pass: a hold keeps the record, a user the memory and the program's file\n");
    return 0;
}
