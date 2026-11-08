/**
 * punchmusl: free large files on an ext2 volume while other processes
 * allocate, write, sync and press the memory, and leave files whose bytes
 * the host checks.
 *
 * Usage: punchmusl NODE ROUNDS, where NODE is the volume and ROUNDS the
 * rounds of the writeback mount, with half as many on the `dirsync` mount
 * after it.
 *
 * The free of a file gives up the big lock at its service points: every
 * 1024 pointers its walk reads and after every round of 512 blocks. A sync
 * gives it up in the sort of a large batch. The program makes those points
 * happen with everything else at once, on the same groups, in one directory:
 *
 * `sparse`, two processes: a file of 6 GiB with a block in each 4 MiB, so
 * that the walk reads a whole pointer block for each; removed open, cut in
 * two steps, cut to nothing, or cut in the middle of a block and kept.
 *
 * `dense`, two processes: a file of 128 MiB written solid; removed open, cut
 * and removed, or cut to a little over a mebibyte and kept.
 *
 * `small`, two processes: files of 1 byte to 4 MiB, written, fsynced, read
 * back, and three of four removed.
 *
 * `press`, one process: takes the free memory and the buffer cache's but for
 * an eighth of the whole, a page at a time, and lets it go.
 *
 * `sync`, one process: on the writeback mount it syncs when a sparse file is
 * done, so that a sync finds thousands of blocks dirty, and on the `dirsync`
 * mount as soon as it can.
 *
 * Every file's bytes come from its stamp and each block's number, and each
 * process reads back what it cut or wrote. The kept files are read whole
 * after each remount, and printed as `PUNCH KEEP NAME SIZE HASH`, after which
 * `PUNCH KEPT N` and the counts in `PUNCH TOTAL`, for tests/cases/machine/
 * punch_scenario.c, which hashes each from the disk image without this
 * kernel. The hash is FNV-1a over each block that is not all zeros in order,
 * its number in eight bytes and then its bytes up to the end of the file.
 * The first sparse process prints `PUNCH ROUND PHASE N` at the end of each
 * of its rounds, which the scenario counts as the case's progress.
 *
 * The counts are fixed; nothing here reads a clock. Prints `PUNCH OK` and
 * exits 0, or prints `PUNCH FAIL: ` with the file and block, and exits 1.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define BLOCK 4096u
#define WORDS (BLOCK / 4u)

#define SPARSE_COUNT  1536u /* blocks of a sparse file */
#define SPARSE_STRIDE 1024u /* blocks between them, a pointer block's worth */
#define SPARSE_JITTER 7u    /* their distance from the stride, a slot in N */
#define DENSE_COUNT   32768u
#define DENSE_KEPT    (1048576ull + 1234ull)
#define SMALL_FILES   32u
#define CHUNK_BLOCKS  16u /* blocks a write of solid data carries */
#define KEEPS_MAX     2048u
#define PRESS_PASSES  4u

#define MOUNT_DIR "/punch"
#define FILES_DIR MOUNT_DIR "/d"

#define FNV_BASIS 14695981039346656037ull
#define FNV_PRIME 1099511628211ull

/* The small files' sizes in bytes, in the order a file's number takes them. */
static const uint32_t small_sizes[] = {1,     100,    4095,   4096,    4097,
                                       65536, 300000, 8193,   1048576, 4194304,
                                       12288, 20000,  131072, 2000000, 40960};

/* What a file holds: `count` blocks from `stamp`, each `stride` blocks from
 * the one before and `jitter` blocks at most past that, up to `size` bytes. */
struct shape {
    uint32_t stamp;
    uint32_t count;
    uint32_t stride;
    uint32_t jitter;
    uint64_t size;
};

/* What a process has done, which it adds to as it goes. */
struct tally {
    uint64_t files;   /* files made */
    uint64_t written; /* bytes written */
    uint64_t freed;   /* bytes of written blocks given back */
    uint64_t logical; /* bytes of file length removed or cut */
    uint64_t cuts;    /* ftruncates */
    uint64_t syncs;   /* sync and fsync calls */
};

/* A record of a process on the pipe to the parent: a kept file, or at its end
 * its tally. */
enum note_kind { NOTE_KEEP, NOTE_TALLY };

struct note {
    uint32_t kind;
    char name[28];
    struct shape shape;
    struct tally tally;
};

/* What each process of a phase is given. */
struct job {
    int phase;           /* 0 on the writeback mount, 1 on dirsync */
    int who;             /* the process's number */
    unsigned rounds;     /* its rounds */
    int notes;           /* the pipe to the parent */
    int tokens;          /* the pipe to the sync process */
    struct tally tally;  /* what it has done */
    unsigned char *room; /* its buffer, of CHUNK_BLOCKS blocks */
};

static struct note keeps[KEEPS_MAX];
static unsigned nkeeps;
static struct tally total;

/** Write the formatted line to standard output in one write. */
static void say(const char *fmt, ...)
{
    char line[200];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n > 0) {
        (void)!write(1, line, (size_t)n < sizeof(line) ? (size_t)n : sizeof(line) - 1);
    }
}

/** Print "PUNCH FAIL: ", what failed and errno, and exit 1. */
static void fail(const char *what, const char *name, uint64_t where)
{
    say("PUNCH FAIL: %s: %s at %llu: errno %d\n", what, name, (unsigned long long)where, errno);
    _exit(1);
}

/** Fill the block at `p` with the bytes of block `block` of file `stamp`. */
static void fill(unsigned char *p, uint32_t stamp, uint64_t block)
{
    uint32_t word = (stamp * 2654435761u) ^ ((uint32_t)block * 40503u) ^ 0x9e3779b9u;
    uint32_t words[WORDS];
    unsigned i;

    for (i = 0; i < WORDS; i++) {
        word = word * 1664525u + 1013904223u;
        words[i] = word;
    }
    memcpy(p, words, BLOCK);
}

/** The next value after `*state` of a generator that has no need of a clock. */
static uint32_t next_random(uint32_t *state)
{
    *state = *state * 1103515245u + 12345u;
    return *state >> 8;
}

/** The block number where block `i` of `s` lies in the file. */
static uint64_t block_of(const struct shape *s, uint32_t i)
{
    return (uint64_t)i * s->stride + (s->jitter ? i % s->jitter : 0);
}

/** How many bytes of block `i` of `s` are in the file, 0 for none. */
static uint32_t bytes_of(const struct shape *s, uint32_t i)
{
    uint64_t at = block_of(s, i) * BLOCK;

    if (at >= s->size) {
        return 0;
    }
    return s->size - at < BLOCK ? (uint32_t)(s->size - at) : BLOCK;
}

/** Add `n` bytes at `p` to the hash `h`. */
static uint64_t hash_bytes(uint64_t h, const unsigned char *p, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++) {
        h = (h ^ p[i]) * FNV_PRIME;
    }
    return h;
}

/** The hash of the file `s`. */
static uint64_t hash_shape(const struct shape *s, unsigned char *room)
{
    uint64_t h = FNV_BASIS;
    uint32_t i;

    for (i = 0; i < s->count; i++) {
        uint32_t n = bytes_of(s, i);
        uint64_t number = block_of(s, i);

        if (n == 0) {
            break;
        }
        fill(room, s->stamp, number);
        h = hash_bytes(h, (const unsigned char *)&number, sizeof(number));
        h = hash_bytes(h, room, n);
    }
    return h;
}

/** Write the whole of the file `s`, named `name`, to `fd`, a request of
 * CHUNK_BLOCKS blocks for adjacent ones, and count it in `job`. */
static void write_shape(struct job *job, int fd, const struct shape *s, const char *name)
{
    uint32_t i = 0;

    while (i < s->count) {
        uint64_t first = block_of(s, i);
        uint32_t n = 0;
        size_t bytes = 0;

        do {
            uint32_t part = bytes_of(s, i + n);

            if (part == 0) {
                break;
            }
            fill(job->room + bytes, s->stamp, block_of(s, i + n));
            bytes += part;
            n++;
        } while (i + n < s->count && n < CHUNK_BLOCKS && block_of(s, i + n) == first + n);
        if (n == 0) {
            break;
        }
        if (pwrite(fd, job->room, bytes, (off_t)(first * BLOCK)) != (ssize_t)bytes) {
            fail("write", name, first);
        }
        job->tally.written += bytes;
        i += n;
    }
}

/** Read every `step`th block of the file `s` from `fd` and compare it, and
 * for a sparse file the block after it, which is a hole. */
static void check_shape(struct job *job, int fd, const struct shape *s, const char *name,
                        uint32_t step)
{
    unsigned char *want = job->room, *got = job->room + BLOCK;
    uint32_t i;

    for (i = 0; i < s->count; i += step) {
        uint32_t n = bytes_of(s, i);
        uint64_t at = block_of(s, i) * BLOCK;

        if (n == 0) {
            break;
        }
        fill(want, s->stamp, block_of(s, i));
        if (pread(fd, got, n, (off_t)at) != (ssize_t)n) {
            fail("read", name, at / BLOCK);
        }
        if (memcmp(got, want, n) != 0) {
            errno = 0;
            fail("bytes differ", name, at / BLOCK);
        }
        if (s->stride > 1 && at + 2 * BLOCK <= s->size) {
            static const unsigned char zeros[BLOCK];

            if (pread(fd, got, BLOCK, (off_t)at + BLOCK) != (ssize_t)BLOCK ||
                memcmp(got, zeros, BLOCK) != 0) {
                errno = 0;
                fail("hole differs", name, at / BLOCK + 1);
            }
        }
    }
}

/** Give the file's path of `name` in `path`. */
static void path_of(char *path, size_t room, const char *name)
{
    snprintf(path, room, FILES_DIR "/%.27s", name);
}

/** Open `name` new for reading and writing; the descriptor. */
static int open_new(const char *name)
{
    char path[64];
    int fd;

    path_of(path, sizeof(path), name);
    fd = open(path, O_CREAT | O_EXCL | O_RDWR, 0644);
    if (fd < 0) {
        fail("create", name, 0);
    }
    return fd;
}

/** Remove the name `name`. */
static void remove_name(const char *name)
{
    char path[64];

    path_of(path, sizeof(path), name);
    if (unlink(path) != 0) {
        fail("unlink", name, 0);
    }
}

/** Count `s`, whose blocks and length are about to go, as `job`'s. */
static void count_gone(struct job *job, const struct shape *s)
{
    uint32_t i;

    job->tally.logical += s->size;
    for (i = 0; i < s->count; i++) {
        job->tally.freed += bytes_of(s, i);
    }
}

/** Cut the file `s` at `fd` to `size`, and count what it gives up. */
static void cut(struct job *job, int fd, struct shape *s, uint64_t size, const char *name)
{
    struct shape left = *s;
    uint32_t i;

    left.size = size;
    job->tally.logical += s->size - size;
    for (i = 0; i < s->count; i++) {
        job->tally.freed += bytes_of(s, i) - bytes_of(&left, i);
    }
    if (ftruncate(fd, (off_t)size) != 0) {
        fail("ftruncate", name, size);
    }
    job->tally.cuts++;
    *s = left;
}

/** Tell the parent that `name`, `s`, stays. */
static void keep(struct job *job, const char *name, const struct shape *s)
{
    struct note n;

    memset(&n, 0, sizeof(n));
    n.kind = NOTE_KEEP;
    snprintf(n.name, sizeof(n.name), "%s", name);
    n.shape = *s;
    if (write(job->notes, &n, sizeof(n)) != (ssize_t)sizeof(n)) {
        fail("note", name, 0);
    }
}

/** Fsync `fd` of `name`. */
static void sync_file(struct job *job, int fd, const char *name)
{
    if (fsync(fd) != 0) {
        fail("fsync", name, 0);
    }
    job->tally.syncs++;
}

/** Tell the sync process that a file's blocks are in the cache. */
static void tell_sync(struct job *job)
{
    (void)!write(job->tokens, "s", 1);
}

/** Round `r` of a sparse process, as the file's comment says. */
static void sparse_round(struct job *job, unsigned r)
{
    struct shape s = {0, SPARSE_COUNT, SPARSE_STRIDE, SPARSE_JITTER, 0};
    char name[28];
    int fd;

    s.stamp = (uint32_t)(job->phase * 1000 + job->who * 100) + r;
    s.size = (block_of(&s, s.count - 1) + 1) * BLOCK;
    snprintf(name, sizeof(name), "s%d-%d-%u", job->phase, job->who, r);
    fd = open_new(name);
    write_shape(job, fd, &s, name);
    job->tally.files++;
    tell_sync(job);
    switch (r % 4) {
    case 0: /* the free comes at the close of the last description */
        remove_name(name);
        check_shape(job, fd, &s, name, 97);
        count_gone(job, &s);
        break;
    case 1:
        cut(job, fd, &s, s.size / 4 * 3, name);
        check_shape(job, fd, &s, name, 53);
        cut(job, fd, &s, s.size / 2, name);
        check_shape(job, fd, &s, name, 31);
        count_gone(job, &s);
        remove_name(name);
        break;
    case 2:
        cut(job, fd, &s, block_of(&s, s.count / 2) * BLOCK + 1000, name);
        check_shape(job, fd, &s, name, 11);
        sync_file(job, fd, name);
        keep(job, name, &s);
        break;
    default:
        cut(job, fd, &s, 0, name);
        remove_name(name);
        break;
    }
    if (close(fd) != 0) {
        fail("close", name, 0);
    }
}

/** Round `r` of a dense process, as the file's comment says. */
static void dense_round(struct job *job, unsigned r)
{
    struct shape s = {0, DENSE_COUNT, 1, 0, (uint64_t)DENSE_COUNT * BLOCK};
    char name[28];
    int fd;

    s.stamp = (uint32_t)(job->phase * 1000 + job->who * 100 + 5000) + r;
    snprintf(name, sizeof(name), "d%d-%d-%u", job->phase, job->who, r);
    fd = open_new(name);
    write_shape(job, fd, &s, name);
    job->tally.files++;
    check_shape(job, fd, &s, name, 257);
    switch (r % 3) {
    case 0:
        remove_name(name);
        count_gone(job, &s);
        break;
    case 1:
        cut(job, fd, &s, s.size / 2 + 777, name);
        check_shape(job, fd, &s, name, 129);
        count_gone(job, &s);
        remove_name(name);
        break;
    default:
        cut(job, fd, &s, DENSE_KEPT, name);
        check_shape(job, fd, &s, name, 1);
        sync_file(job, fd, name);
        keep(job, name, &s);
        break;
    }
    if (close(fd) != 0) {
        fail("close", name, 0);
    }
}

/** Make small file `f` of round `r`, its shape in `*s` and name in `name`. */
static void small_file(struct job *job, unsigned r, unsigned f, struct shape *s, char *name)
{
    uint32_t size;
    int fd;

    size = small_sizes[(job->who + r * 7 + f) % (sizeof(small_sizes) / sizeof(small_sizes[0]))];
    s->stamp = (uint32_t)(job->phase * 100000 + job->who * 10000 + r * 100 + f + 10000);
    s->count = (size + BLOCK - 1) / BLOCK;
    s->stride = 1;
    s->jitter = 0;
    s->size = size;
    snprintf(name, 28, "f%d-%d-%u-%u", job->phase, job->who, r, f);
    fd = open_new(name);
    write_shape(job, fd, s, name);
    sync_file(job, fd, name);
    job->tally.files++;
    if (close(fd) != 0) {
        fail("close", name, 0);
    }
}

/** Read the file `name`, `s`, back, and remove it unless `kept`. */
static void small_end(struct job *job, const struct shape *s, const char *name, int kept)
{
    char path[64];
    int fd;

    path_of(path, sizeof(path), name);
    fd = open(path, O_RDONLY);
    if (fd < 0) {
        fail("open", name, 0);
    }
    check_shape(job, fd, s, name, 1);
    if (close(fd) != 0) {
        fail("close", name, 0);
    }
    if (kept) {
        keep(job, name, s);
    } else {
        count_gone(job, s);
        remove_name(name);
    }
}

/** Round `r` of a small process, as the file's comment says. */
static void small_round(struct job *job, unsigned r)
{
    struct shape shapes[SMALL_FILES];
    char names[SMALL_FILES][28];
    unsigned f;

    for (f = 0; f < SMALL_FILES; f++) {
        small_file(job, r, f, &shapes[f], names[f]);
    }
    for (f = 0; f < SMALL_FILES; f++) {
        small_end(job, &shapes[f], names[f], f % 4 == 3);
    }
}

/** The number in kB of the line of /proc/meminfo that begins with `key`, or
 * -1. */
static long meminfo(const char *key)
{
    char line[80];
    FILE *f = fopen("/proc/meminfo", "r");
    long value = -1;

    while (f && fgets(line, sizeof(line), f)) {
        if (strncmp(line, key, strlen(key)) == 0) {
            value = atol(line + strlen(key));
        }
    }
    if (f) {
        fclose(f);
    }
    return value;
}

/** A round of the press process: the free memory and the buffer cache's but
 * for an eighth of the whole taken, a page at a time, and let go, each pass.
 * A map refused passes; the machine's room is not what this tests. */
static void press_round(struct job *job)
{
    long total_kb = meminfo("MemTotal:");
    unsigned pass;

    (void)job;
    for (pass = 0; pass < PRESS_PASSES; pass++) {
        long free_kb = meminfo("MemFree:");
        long buffers_kb = meminfo("Buffers:");
        long take_kb = free_kb + buffers_kb - total_kb / 8;
        size_t bytes, at;
        volatile unsigned char *mem;

        if (take_kb <= 0) {
            continue;
        }
        bytes = (size_t)take_kb * 1024u;
        mem = mmap(0, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mem == MAP_FAILED) {
            continue;
        }
        for (at = 0; at < bytes; at += 4096) {
            mem[at] = 1;
        }
        munmap((void *)mem, bytes);
    }
}

/** Sync until the last writer has gone: at each file a sparse process has
 * done, or with `fast` as often as it can, yielding between. */
static void sync_loop(struct job *job, int fast)
{
    char token;

    if (fast) {
        (void)fcntl(job->tokens, F_SETFL, O_NONBLOCK);
    }
    for (;;) {
        ssize_t got = read(job->tokens, &token, 1);

        if (got == 0 || (got < 0 && errno != EAGAIN && errno != EINTR)) {
            return;
        }
        sync();
        job->tally.syncs++;
        if (fast) {
            (void)sched_yield();
        }
    }
}

/** Send `job`'s tally to the parent. */
static void send_tally(struct job *job)
{
    struct note n;

    memset(&n, 0, sizeof(n));
    n.kind = NOTE_TALLY;
    n.tally = job->tally;
    if (write(job->notes, &n, sizeof(n)) != (ssize_t)sizeof(n)) {
        fail("note", "tally", 0);
    }
}

/* The processes of a phase, in the order of their numbers. */
enum role { ROLE_SPARSE, ROLE_DENSE, ROLE_SMALL, ROLE_PRESS, ROLE_SYNC };

static const enum role roles[] = {ROLE_SPARSE, ROLE_SPARSE, ROLE_DENSE, ROLE_DENSE,
                                  ROLE_SMALL,  ROLE_SMALL,  ROLE_PRESS, ROLE_SYNC};
#define ROLES (sizeof(roles) / sizeof(roles[0]))

/** Be the process `job`, of role `role`, to its end. */
static void work(struct job *job, enum role role, int fast)
{
    unsigned r;

    job->room = malloc((CHUNK_BLOCKS + 1) * BLOCK);
    if (!job->room) {
        fail("malloc", "room", 0);
    }
    if (role == ROLE_SYNC) {
        sync_loop(job, fast);
    }
    for (r = 0; role != ROLE_SYNC && r < job->rounds; r++) {
        switch (role) {
        case ROLE_SPARSE:
            sparse_round(job, r);
            break;
        case ROLE_DENSE:
            dense_round(job, r);
            break;
        case ROLE_SMALL:
            small_round(job, r);
            break;
        default:
            press_round(job);
            break;
        }
        if (job->who == 0) {
            say("PUNCH ROUND %d %u\n", job->phase, r + 1);
        }
    }
    send_tally(job);
    _exit(0);
}

/** Add `add` to the program's tally. */
static void tally_add(const struct tally *add)
{
    total.files += add->files;
    total.written += add->written;
    total.freed += add->freed;
    total.logical += add->logical;
    total.cuts += add->cuts;
    total.syncs += add->syncs;
}

/** Read the processes' notes to their end, into `keeps` and the tally. */
static void read_notes(int fd)
{
    struct note n;

    while (read(fd, &n, sizeof(n)) == (ssize_t)sizeof(n)) {
        if (n.kind == NOTE_TALLY) {
            tally_add(&n.tally);
        } else if (nkeeps == KEEPS_MAX) {
            errno = 0;
            fail("kept files", n.name, nkeeps);
        } else {
            keeps[nkeeps++] = n;
        }
    }
}

/** Run a phase of `rounds` rounds on the mounted volume: start its
 * processes, read their notes and wait for them all. */
static void run_phase(int phase, unsigned rounds, int fast)
{
    int notes[2], tokens[2];
    pid_t pids[ROLES];
    unsigned i;

    if (pipe(notes) != 0 || pipe(tokens) != 0) {
        fail("pipe", "phase", (uint64_t)phase);
    }
    for (i = 0; i < ROLES; i++) {
        pids[i] = fork();
        if (pids[i] < 0) {
            fail("fork", "phase", i);
        }
        if (pids[i] == 0) {
            struct job job;

            memset(&job, 0, sizeof(job));
            job.phase = phase;
            job.who = (int)i;
            job.rounds = rounds;
            job.notes = notes[1];
            job.tokens = roles[i] == ROLE_SYNC ? tokens[0] : tokens[1];
            close(notes[0]);
            close(roles[i] == ROLE_SYNC ? tokens[1] : tokens[0]);
            work(&job, roles[i], fast);
        }
    }
    close(notes[1]);
    close(tokens[0]);
    close(tokens[1]);
    read_notes(notes[0]);
    close(notes[0]);
    for (i = 0; i < ROLES; i++) {
        int status = 0;

        if (waitpid(pids[i], &status, 0) != pids[i] || !WIFEXITED(status) ||
            WEXITSTATUS(status) != 0) {
            errno = 0;
            fail("a process failed", "phase", (uint64_t)status);
        }
    }
}

/** Read every kept file whole, as `job` of no phase, against its shape. */
static void check_keeps(unsigned char *room)
{
    struct job job;
    unsigned i;

    memset(&job, 0, sizeof(job));
    job.room = room;
    for (i = 0; i < nkeeps; i++) {
        char path[64];
        struct stat st;
        int fd;

        path_of(path, sizeof(path), keeps[i].name);
        fd = open(path, O_RDONLY);
        if (fd < 0) {
            fail("open kept", keeps[i].name, 0);
        }
        if (fstat(fd, &st) != 0 || (uint64_t)st.st_size != keeps[i].shape.size) {
            errno = 0;
            fail("kept size", keeps[i].name, (uint64_t)st.st_size);
        }
        check_shape(&job, fd, &keeps[i].shape, keeps[i].name, 1);
        close(fd);
    }
}

/** Mount the volume `node` with `flags` on MOUNT_DIR. */
static void mount_volume(const char *node, unsigned long flags)
{
    if (mount(node, MOUNT_DIR, "ext2", flags, 0) != 0) {
        fail("mount", node, flags);
    }
}

/** Sync and unmount the volume. */
static void unmount_volume(void)
{
    sync();
    if (umount(MOUNT_DIR) != 0) {
        fail("umount", MOUNT_DIR, 0);
    }
}

/** Print every kept file and the counts, as the file's comment says. */
static void print_keeps(unsigned char *room)
{
    unsigned i;

    for (i = 0; i < nkeeps; i++) {
        say("PUNCH KEEP %s %llu %016llx\n", keeps[i].name, (unsigned long long)keeps[i].shape.size,
            (unsigned long long)hash_shape(&keeps[i].shape, room));
    }
    say("PUNCH KEPT %u\n", nkeeps);
    say("PUNCH TOTAL files %llu written %llu freed %llu logical %llu cuts %llu syncs %llu\n",
        (unsigned long long)total.files, (unsigned long long)total.written,
        (unsigned long long)total.freed, (unsigned long long)total.logical,
        (unsigned long long)total.cuts, (unsigned long long)total.syncs);
}

/** The phases, each remount's reading of the kept files, and the last print. */
int main(int argc, char **argv)
{
    unsigned char *room = malloc((CHUNK_BLOCKS + 1) * BLOCK);
    unsigned rounds;

    if (argc != 3 || !room) {
        say("PUNCH FAIL: usage: punchmusl NODE ROUNDS\n");
        return 1;
    }
    rounds = (unsigned)atoi(argv[2]);
    if ((mkdir(MOUNT_DIR, 0755) != 0 && errno != EEXIST)) {
        fail("mkdir", MOUNT_DIR, 0);
    }
    mount_volume(argv[1], 0);
    if (mkdir(FILES_DIR, 0755) != 0) {
        fail("mkdir", FILES_DIR, 0);
    }
    run_phase(0, rounds, 0);
    unmount_volume();
    say("punch: writeback phase done, %u files kept\n", nkeeps);
    mount_volume(argv[1], MS_DIRSYNC);
    check_keeps(room);
    run_phase(1, rounds / 2u, 1);
    unmount_volume();
    say("punch: dirsync phase done, %u files kept\n", nkeeps);
    mount_volume(argv[1], 0);
    check_keeps(room);
    print_keeps(room);
    unmount_volume();
    say("PUNCH OK\n");
    return 0;
}
