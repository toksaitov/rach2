/**
 * Check System V detach errors and metadata against a model unmapper.
 *
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-cflags: -Wno-int-to-pointer-cast -Wno-pointer-to-int-cast
 *
 * Unused exports are declared private before including sysv.c, so the host
 * compiler discards them. The unmapper reports a missing attachment, an
 * allocation failure or success; the real detach search and reaper run.
 */
#include "lib/kint.h"
struct isr_params;
struct tgroup;
struct ipcns;
struct sysv_row;
static long sys_ipc(struct isr_params *params);
static long sys_semop(struct isr_params *params);
static long sys_semget(struct isr_params *params);
static long sys_semctl(struct isr_params *params);
static long sys_shmget(struct isr_params *params);
static long sys_shmctl(struct isr_params *params);
static long sys_shmat(struct isr_params *params);
static long sys_shmdt(struct isr_params *params);
static long sys_msgget(struct isr_params *params);
static long sys_msgsnd(struct isr_params *params);
static long sys_msgrcv(struct isr_params *params);
static long sys_msgctl(struct isr_params *params);
static long sys_semtimedop_time64(struct isr_params *params);
static int sysv_ipc_restartable(const struct isr_params *params);
static void sysv_exit(struct tgroup *tg);
static int sysv_row_at(int kind, int i, struct sysv_row *row);
static void ipc_ns_gone(struct ipcns *ns);
#include "ipc/sysv.c"
#include "mm/shmem.h"

extern int printf(const char *format, ...);
extern int dprintf(int fd, const char *format, ...);

static process_t caller;
struct percpu cpu_table[NR_CPUS] = {{.current = &caller}};
static struct shm_seg segments[3];
static struct open_file files[3];
static struct shmem memory[3];
static int replies[3], unmaps, releases, frees, attachment_reads;
static int failures;

#define ATTACHMENT  0x4000000u
#define DETACH_TIME 123
#define LAST_PID    42

#define CHECK(condition, detail)                                      \
    do {                                                              \
        if (!(condition)) {                                           \
            dprintf(2, "sysv-detach-hosttest: failed: %s\n", detail); \
            failures++;                                               \
        }                                                             \
    } while (0)

/** The model file's node is its shared-memory record. */
struct shmem *tmpfs_shmem(void *node)
{
    return node;
}

/** Count the unmap attempt and return the chosen result for its segment. */
int32_t munmap_shm(uintptr_t addr, const struct shmem *sh, size_t size)
{
    int i = (int)(sh - memory);

    CHECK(addr == ATTACHMENT && size == PAGE_SIZE, "unmap arguments");
    unmaps++;
    return replies[i];
}

/** A removed segment has no attachment after a successful detach. */
uint32_t shm_attaches(const struct shmem *sh)
{
    (void)sh;
    attachment_reads++;
    return 0;
}

/** A fixed clock makes a successful detach's timestamp observable. */
void get_current_time(int64_t *seconds, int64_t *nanoseconds)
{
    *seconds = DETACH_TIME;
    *nanoseconds = 0;
}

/** Count the reaper's release of a segment's file. */
void fd_ofd_unref(struct open_file *ofd)
{
    CHECK(ofd == &files[1], "only the detached segment's file is released");
    releases++;
}

/** Count the reaper's release of the model segment, whose storage is static. */
void kfree(void *p)
{
    CHECK(p == &segments[1], "only the detached segment is freed");
    frees++;
}

/** Populate three nonadjacent slots; the middle segment is marked removed. */
static void reset(void)
{
    int i;

    k_memset(tables, 0, sizeof(tables));
    k_memset(segments, 0, sizeof(segments));
    k_memset(files, 0, sizeof(files));
    unmaps = releases = frees = attachment_reads = 0;
    caller.tgid = LAST_PID;
    for (i = 0; i < 3; i++) {
        int place = 1 + i * 6;

        segments[i].perm.place = place;
        segments[i].size = PAGE_SIZE;
        segments[i].ofd = &files[i];
        segments[i].dtime = 17;
        segments[i].lpid = 19;
        files[i].vnode = &memory[i];
        tables[SYSV_SHM][place] = &segments[i].perm;
        replies[i] = -EINVAL;
    }
    segments[1].perm.mode = SHM_DEST;
}

/** An error changes neither detach metadata nor the removed segment's life. */
static void check_unchanged(void)
{
    CHECK(segments[1].dtime == 17 && segments[1].lpid == 19,
          "failed detach preserves time and last operator");
    CHECK(tables[SYSV_SHM][7] == &segments[1].perm && !segments[1].perm.removed &&
              attachment_reads == 0 && releases == 0 && frees == 0,
          "failed detach neither reaps nor releases a segment");
}

/** Run the search, error and successful-detach cases; 0 if all pass. */
int main(void)
{
    reset();
    CHECK(shm_dt(ATTACHMENT + 1) == -EINVAL && unmaps == 0,
          "unaligned address fails before the search");
    check_unchanged();

    reset();
    CHECK(shm_dt(ATTACHMENT) == -EINVAL && unmaps == 3,
          "missing attachment searches every occupied slot");
    check_unchanged();

    reset();
    replies[1] = -ENOMEM;
    replies[2] = 0;
    CHECK(shm_dt(ATTACHMENT) == -ENOMEM && unmaps == 2,
          "unmap ENOMEM is returned without searching later slots");
    check_unchanged();

    reset();
    replies[1] = 0;
    CHECK(shm_dt(ATTACHMENT) == 0 && unmaps == 2,
          "successful detach skips the preceding nonattachment");
    CHECK(segments[1].dtime == DETACH_TIME && segments[1].lpid == LAST_PID,
          "successful detach records time and last operator");
    CHECK(tables[SYSV_SHM][7] == NULL && segments[1].perm.removed && attachment_reads == 1 &&
              releases == 1 && frees == 1,
          "successful detach reaps the removed segment once");
    if (failures) {
        return 1;
    }
    printf("sysv-detach-hosttest: result: pass: detach errors and metadata\n");
    return 0;
}
