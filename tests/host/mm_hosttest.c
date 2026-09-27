/**
 * Check the two counts of an address space: the users, whose last put frees
 * the memory, and the holds, which keep only the record.
 *
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 *
 * `mm/mm.c` is included whole, over a model of the paging layer that counts
 * what is freed. A hold, which a /proc/PID/mem descriptor takes, must keep
 * the record and nothing else: the last user's put frees the pages, the
 * directory and the regions while the hold remains, a holder then takes no
 * user's count, and the last hold frees the record, which AddressSanitizer
 * judges by any use after.
 */
#include "mm/mm.c"

extern void *calloc(unsigned long count, unsigned long size);
extern void free(void *ptr);
extern int printf(const char *format, ...);

page_dir_t *_kernel_dir;
page_dir_t *_curr_dir;
static page_dir_t kernel_dir;
static int dirs, pages_freed, dirs_freed, regions_freed, reaps, rows, failures;

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
    CHECK(pages_freed == 0 && dirs_freed == 0 && mm->dir != NULL,
          "whose put, not the last, frees nothing");
    mm_put(mm);
    CHECK(pages_freed == 1 && dirs_freed == 1, "and the last user's put frees the memory");
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

int main(void)
{
    _kernel_dir = &kernel_dir;
    _curr_dir = _kernel_dir;
    hold_keeps_record();
    live_user();
    drop_switches();
    CHECK(rows == 3 && dirs == 3, "every row ran");
    if (failures) {
        return 1;
    }
    printf("mm-test: pass: a hold keeps the record, a user the memory\n");
    return 0;
}
