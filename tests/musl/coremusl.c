/**
 * Core files, made in /dev/shm, a tmpfs that keeps the holes of a file.
 *
 * A child that raises its soft RLIMIT_CORE and aborts leaves "core" in its
 * working directory, where a symbolic link had the name: the parent's wait
 * reports SIGABRT with WCOREDUMP, the link's target is never created, and
 * the file is an ELF core of the architecture, i386 or x86-64, whose
 * NT_PRSTATUS names the child and SIGABRT, whose NT_PRPSINFO has the program's
 * name, whose NT_AUXV has the page size, and whose PT_LOAD for the child's data
 * has the word the child stored there. A child whose soft limit is two pages
 * leaves no WCOREDUMP and a file of its own, with the child's notes, that stops
 * before its last segment ends and has no more than two pages of bytes that are
 * not zero, since only a hole reads as zeros without being written. One whose
 * limit is 0, and one that is not dumpable, leave no file and no WCOREDUMP.
 *
 * A dump whose last readable region ends in a page never touched is given
 * a zero at its last byte, so the file has its whole length, and that zero
 * counts toward the limit. Two children show it. Each maps two pages over
 * the top of its stack, marks the first and leaves the second untouched,
 * and maps the stack's top page, where its arguments are, with no access,
 * all from a handler on an alternate stack; it then counts the bytes its
 * dump writes before that zero, from /proc/self/maps and mincore, and sets
 * its soft limit to one byte less than the count, or to the count, each in
 * a directory of its own. Both run before either file is read, so that the
 * parent forks them from one state and their counts agree. The first file
 * ends before the marked page, which shows the count is not more than the
 * dump writes; the second has the marked page and ends with it, and its wait
 * status has no WCOREDUMP, since the zero no longer fits.
 *
 * Prints "COREMUSL OK" on success and "COREMUSL FAIL (detail)" otherwise.
 */

#define _GNU_SOURCE
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define DIR_PATH    "/dev/shm/coremusl.d"
#define CORE_PATH   DIR_PATH "/core"
#define TARGET_PATH DIR_PATH "/target"
#define MARK        0x5ca1ab1eu

/*
 * A core file's layout on each architecture: its ELF class and machine, and
 * the offsets of what the rows read in the notes, NT_PRSTATUS's signal and
 * pid and NT_PRPSINFO's program name, whose structs Linux lays out with
 * longs; NT_AUXV is pairs of longs. STACK_TOP is the end of the stack, the
 * kernel at 3 GiB on i386 and the last page of the lower half on x86-64.
 */
#if defined __x86_64__
typedef Elf64_Ehdr Ehdr;
typedef Elf64_Phdr Phdr;
typedef Elf64_Nhdr Nhdr;
#define ELF_CLASS     ELFCLASS64
#define ELF_MACHINE   EM_X86_64
#define PRSTATUS_SIZE 336
#define PR_PID        32
#define PRPSINFO_SIZE 136
#define PR_FNAME      40
#define STACK_TOP     0x00007FFFFFFFF000ul
#else
typedef Elf32_Ehdr Ehdr;
typedef Elf32_Phdr Phdr;
typedef Elf32_Nhdr Nhdr;
#define ELF_CLASS     ELFCLASS32
#define ELF_MACHINE   EM_386
#define PRSTATUS_SIZE 144
#define PR_PID        24
#define PRPSINFO_SIZE 124
#define PR_FNAME      28
#define STACK_TOP     0xC0000000u
#endif
#define PR_CURSIG 12

/* The pages `hole_dump` maps over the top of the stack: two readable ones,
 * the first marked and the second never touched, and the top page, where
 * the arguments are, with no access, so that the dump reads nothing there. */
#define HOLE_BASE  (STACK_TOP - 3u * 4096u)
#define HOLE_FILL  0xA5
#define ARGS_FLOOR (STACK_TOP - 4096u)

static volatile uint32_t marker;

/* The bytes of the note segment of a process of one thread, which the first
 * core file has; the address of the first argument; the pipe a counting
 * child sends its count through, and what it takes off the count for its
 * limit; and what its count is read into, touched before it counts. */
static uint32_t note_bytes;
static uintptr_t first_arg;
static int count_pipe[2];
static rlim_t count_less;
static char alt_stack[65536];
static char maps_text[16384];
static unsigned char in_core[1024];

static int fail(const char *what)
{
    printf("COREMUSL FAIL (%s: errno %d)\n", what, errno);
    return 1;
}

/** Fork a child that sets its soft core limit to `limit`, and with
 * `dumpable` 0 clears its dumpable flag, stores the mark and aborts; the
 * child's pid, with its wait status in `*status`. */
static pid_t abort_child(rlim_t limit, int dumpable, int *status)
{
    pid_t pid = fork();

    if (pid == 0) {
        struct rlimit rl;

        if (chdir(DIR_PATH) != 0 || getrlimit(RLIMIT_CORE, &rl) != 0) {
            _exit(2);
        }
        rl.rlim_cur = limit;
        if (setrlimit(RLIMIT_CORE, &rl) != 0 ||
            (!dumpable && prctl(PR_SET_DUMPABLE, 0, 0, 0, 0) != 0)) {
            _exit(3);
        }
        marker = MARK;
        abort();
    }
    if (pid < 0 || waitpid(pid, status, 0) != pid) {
        return -1;
    }
    return pid;
}

/** Read the whole file at `path` into a buffer from the heap; its size goes
 * to `*size`. NULL when it cannot be read. */
static unsigned char *slurp(const char *path, size_t *size)
{
    struct stat st;
    unsigned char *buf;
    size_t done = 0;
    int fd = open(path, O_RDONLY);

    if (fd < 0 || fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) ||
        !(buf = malloc(st.st_size ? st.st_size : 1))) {
        return NULL;
    }
    while (done < (size_t)st.st_size) {
        ssize_t n = read(fd, buf + done, st.st_size - done);

        if (n <= 0) {
            free(buf);
            close(fd);
            return NULL;
        }
        done += n;
    }
    close(fd);
    *size = done;
    return buf;
}

/** Check the notes of `seg`: the child's status and signal, its name and
 * its page size. */
static int notes_check(const unsigned char *core, const Phdr *seg, pid_t child)
{
    const unsigned char *p = core + seg->p_offset;
    const unsigned char *end = p + seg->p_filesz;
    int status = 0, info = 0, auxv = 0;

    while (p + 12 <= end) {
        const Nhdr *n = (const Nhdr *)p;
        const unsigned char *desc = p + 12 + ((n->n_namesz + 3) & ~3u);

        if (n->n_type == NT_PRSTATUS && n->n_descsz == PRSTATUS_SIZE && !status) {
            int16_t cursig;
            int32_t pid;

            memcpy(&cursig, desc + PR_CURSIG, sizeof(cursig));
            memcpy(&pid, desc + PR_PID, sizeof(pid));
            status = cursig == SIGABRT && pid == child;
        } else if (n->n_type == NT_PRPSINFO && n->n_descsz == PRPSINFO_SIZE) {
            info = strncmp((const char *)desc + PR_FNAME, "coremusl", 16) == 0;
        } else if (n->n_type == NT_AUXV) {
            const unsigned long *a = (const unsigned long *)desc;
            uint32_t i;

            for (i = 0; i + 1 < n->n_descsz / sizeof(*a) && a[i] != AT_NULL; i += 2) {
                if (a[i] == AT_PAGESZ) {
                    auxv = a[i + 1] == 4096;
                }
            }
        }
        p = desc + ((n->n_descsz + 3) & ~3u);
    }
    if (!status || !info || !auxv) {
        errno = 0;
        return fail(!status ? "NT_PRSTATUS" : !info ? "NT_PRPSINFO" : "NT_AUXV");
    }
    return 0;
}

/** Check the core file of `child`: an ELF core of the architecture whose
 * notes are right and whose memory has the mark at the marker's address. */
static int core_check(pid_t child)
{
    size_t size;
    unsigned char *core = slurp(CORE_PATH, &size);
    const Ehdr *eh = (const Ehdr *)core;
    uintptr_t at = (uintptr_t)&marker;
    int notes = 0, found = 0;
    int i;

    if (!core || size < sizeof(*eh)) {
        return fail("reading the core file");
    }
    errno = 0;
    if (memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0 || eh->e_ident[EI_CLASS] != ELF_CLASS ||
        eh->e_type != ET_CORE || eh->e_machine != ELF_MACHINE || eh->e_phentsize != sizeof(Phdr) ||
        eh->e_phoff + (size_t)eh->e_phnum * sizeof(Phdr) > size) {
        return fail("the ELF header");
    }
    for (i = 0; i < eh->e_phnum; i++) {
        const Phdr *ph = (const Phdr *)(core + eh->e_phoff) + i;

        if ((size_t)ph->p_offset + ph->p_filesz > size) {
            return fail("a segment past the end of the file");
        }
        if (ph->p_type == PT_NOTE) {
            if (notes++ != 0 || notes_check(core, ph, child) != 0) {
                return notes > 1 ? fail("two note segments") : 1;
            }
            note_bytes = ph->p_filesz;
        } else if (ph->p_type == PT_LOAD && at >= ph->p_vaddr &&
                   at + 4 <= ph->p_vaddr + ph->p_filesz) {
            uint32_t word;

            memcpy(&word, core + ph->p_offset + (at - ph->p_vaddr), sizeof(word));
            found = word == MARK;
        }
    }
    free(core);
    if (!notes || !found) {
        return fail(!notes ? "no note segment" : "the mark in the child's data");
    }
    return 0;
}

/** Check the core file a child whose limit was `limit` bytes left: its
 * notes are the child's, it stops before its last segment ends, and no more
 * than `limit` of its bytes are not zero. */
static int short_check(pid_t child, size_t limit)
{
    size_t size, i, end = 0, written = 0;
    unsigned char *core = slurp(CORE_PATH, &size);
    const Ehdr *eh = (const Ehdr *)core;
    int notes = 0;
    int k;

    if (!core || size < sizeof(*eh)) {
        return fail("reading the short core file");
    }
    errno = 0;
    if (memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0 || eh->e_type != ET_CORE ||
        eh->e_phoff + (size_t)eh->e_phnum * sizeof(Phdr) > size) {
        return fail("the short core's ELF header");
    }
    for (k = 0; k < eh->e_phnum; k++) {
        const Phdr *ph = (const Phdr *)(core + eh->e_phoff) + k;

        if (ph->p_type == PT_NOTE) {
            if ((size_t)ph->p_offset + ph->p_filesz > size) {
                return fail("the short core's notes cut short");
            }
            if (notes_check(core, ph, child) != 0) {
                return 1;
            }
            notes = 1;
        } else if (ph->p_type == PT_LOAD && (size_t)ph->p_offset + ph->p_filesz > end) {
            end = (size_t)ph->p_offset + ph->p_filesz;
        }
    }
    for (i = 0; i < size; i++) {
        written += core[i] != 0;
    }
    free(core);
    if (!notes || size >= end || written > limit) {
        return fail(!notes        ? "no note segment in the short core"
                    : size >= end ? "a short core with every segment whole"
                                  : "a short core with more bytes than its limit");
    }
    return 0;
}

/**
 * The bytes the dump of this process writes before the zero at the file's
 * end, holes excepted: the ELF header, a program header for the notes and
 * one for each region, the notes, and each page of a readable region that
 * is in memory, the pages `core_write` does not leave as holes. The regions
 * come from /proc/self/maps and the pages from mincore. 0 when either
 * cannot be read.
 */
static uint32_t dump_bytes(void)
{
    uint32_t regions = 0, pages = 0;
    char *line = maps_text;
    ssize_t len;
    int fd = open("/proc/self/maps", O_RDONLY);

    if (fd < 0) {
        return 0;
    }
    len = read(fd, maps_text, sizeof(maps_text) - 1);
    close(fd);
    if (len <= 0) {
        return 0;
    }
    maps_text[len] = '\0';
    while (line && *line) {
        unsigned long start = strtoul(line, &line, 16);
        unsigned long end = strtoul(line + 1, &line, 16);
        unsigned long at;

        regions++;
        for (at = start; line[1] == 'r' && at < end; at += sizeof(in_core) * 4096ul) {
            unsigned long n = (end - at) / 4096ul;
            unsigned long i;

            if (n > sizeof(in_core)) {
                n = sizeof(in_core);
            }
            if (mincore((void *)at, n * 4096ul, in_core) != 0) {
                return 0;
            }
            for (i = 0; i < n; i++) {
                pages += in_core[i] & 1u;
            }
        }
        line = strchr(line, '\n');
        line = line ? line + 1 : NULL;
    }
    return sizeof(Ehdr) + sizeof(Phdr) * (regions + 1u) + note_bytes + 4096u * pages;
}

/**
 * The handler of the counting child's SIGUSR1, on the alternate stack:
 * rebuild the top of the stack, which nothing runs on now, count the bytes
 * the dump will write before its zero, send the count to the parent, set the
 * soft RLIMIT_CORE to it less `count_less`, and abort.
 */
static void hole_dump(int sig)
{
    struct rlimit rl;
    uint32_t n;

    (void)sig;
    if (mmap((void *)HOLE_BASE, 2u * 4096u, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == MAP_FAILED ||
        mmap((void *)ARGS_FLOOR, 4096u, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1,
             0) == MAP_FAILED) {
        _exit(4);
    }
    memset((void *)HOLE_BASE, HOLE_FILL, 4096);
    n = dump_bytes();
    if (n == 0 || write(count_pipe[1], &n, sizeof(n)) != (ssize_t)sizeof(n) ||
        getrlimit(RLIMIT_CORE, &rl) != 0) {
        _exit(5);
    }
    rl.rlim_cur = n - count_less;
    if (setrlimit(RLIMIT_CORE, &rl) != 0) {
        _exit(6);
    }
    abort();
}

/**
 * Fork a child that counts the bytes its dump writes before the zero at its
 * end and aborts in the directory `dir` with a limit of the count less
 * `less`, as `hole_dump` does, its arguments being in the stack's top page;
 * the child's pid, with its wait status in `*status` and its count in
 * `*counted`, 0 when it sent none.
 */
static pid_t hole_child(const char *dir, rlim_t less, int *status, uint32_t *counted)
{
    pid_t pid;

    *counted = 0;
    if (pipe(count_pipe) != 0) {
        return -1;
    }
    count_less = less;
    pid = fork();
    if (pid == 0) {
        struct sigaction sa;
        stack_t ss;

        memset(alt_stack, 0, sizeof(alt_stack));
        memset(maps_text, 0, sizeof(maps_text));
        memset(in_core, 0, sizeof(in_core));
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = hole_dump;
        sa.sa_flags = SA_ONSTACK;
        ss.ss_sp = alt_stack;
        ss.ss_size = sizeof(alt_stack);
        ss.ss_flags = 0;
        close(count_pipe[0]);
        if (first_arg < ARGS_FLOOR || chdir(dir) != 0 || sigaltstack(&ss, NULL) != 0 ||
            sigaction(SIGUSR1, &sa, NULL) != 0) {
            _exit(2);
        }
        kill(getpid(), SIGUSR1);
        _exit(3);
    }
    close(count_pipe[1]);
    if (pid > 0 && read(count_pipe[0], counted, sizeof(*counted)) != (ssize_t)sizeof(*counted)) {
        *counted = 0;
    }
    close(count_pipe[0]);
    if (pid < 0 || waitpid(pid, status, 0) != pid) {
        return -1;
    }
    return pid;
}

/**
 * Whether the core file at `path` ends with the marked page after the pages
 * before it: 1 when its size is at least the marked page's end and that page
 * is all the mark, 0 when it is not, -1 when the file is no ELF core. The
 * file's size goes to `*size` and the length its program headers give it to
 * `*whole`.
 */
static int marked_page(const char *path, size_t *size, size_t *whole)
{
    unsigned char *core = slurp(path, size);
    const Ehdr *eh = (const Ehdr *)core;
    size_t end = 0, i;
    int k, marked = 0;

    if (!core || *size < sizeof(*eh) || memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0 ||
        eh->e_phoff + (size_t)eh->e_phnum * sizeof(Phdr) > *size) {
        free(core);
        return -1;
    }
    for (k = 0; k < eh->e_phnum; k++) {
        const Phdr *ph = (const Phdr *)(core + eh->e_phoff) + k;

        if (ph->p_type == PT_LOAD && (size_t)ph->p_offset + ph->p_filesz > end) {
            end = (size_t)ph->p_offset + ph->p_filesz;
        }
    }
    *whole = end;
    if (end >= 2u * 4096u && *size >= end - 4096u) {
        marked = 1;
        for (i = end - 2u * 4096u; i < end - 4096u; i++) {
            marked &= core[i] == HOLE_FILL;
        }
    }
    free(core);
    return marked;
}

/**
 * The limit counts the zero a dump that ends in a hole writes at its last
 * byte: the child whose limit is one byte less than its count leaves a file
 * that ends before its marked page, and the child whose limit is its count
 * leaves one that ends with that page, with no WCOREDUMP.
 */
static int hole_row(void)
{
    static const char *const dirs[2] = {DIR_PATH "/short", DIR_PATH "/exact"};
    static const char *const cores[2] = {DIR_PATH "/short/core", DIR_PATH "/exact/core"};
    uint32_t counted[2];
    size_t size[2], whole[2];
    int status[2], marked[2];
    int i, ran = 1;

    for (i = 0; i < 2; i++) {
        unlink(cores[i]);
        if (mkdir(dirs[i], 0700) != 0 && errno != EEXIST) {
            return fail("the directories of the counting children");
        }
    }
    for (i = 0; i < 2; i++) {
        ran &= hole_child(dirs[i], i == 0 ? 1 : 0, &status[i], &counted[i]) > 0 &&
               counted[i] != 0 && WIFSIGNALED(status[i]) && WTERMSIG(status[i]) == SIGABRT;
    }
    for (i = 0; i < 2; i++) {
        marked[i] = ran ? marked_page(cores[i], &size[i], &whole[i]) : -1;
        unlink(cores[i]);
        rmdir(dirs[i]);
    }
    if (!ran) {
        return fail("a child that rebuilt its stack's top and aborted");
    }
    errno = 0;
    if (counted[0] != counted[1] || WCOREDUMP(status[0]) || marked[0] != 0 || marked[1] != 1) {
        return fail("the limit a child counted from its regions");
    }
    if (WCOREDUMP(status[1]) || size[1] != whole[1] - 4096u) {
        return fail("a limit of the bytes before the zero at the end took the zero");
    }
    return 0;
}

/** The children that leave no core file: one whose limit is 0 and one that
 * is not dumpable. */
static int no_core_rows(void)
{
    int status;
    pid_t child = abort_child(0, 1, &status);

    if (child < 0 || !WIFSIGNALED(status) || WCOREDUMP(status) || access(CORE_PATH, F_OK) == 0) {
        return fail("a core with the limit 0");
    }
    child = abort_child(RLIM_INFINITY, 0, &status);
    if (child < 0 || !WIFSIGNALED(status) || WCOREDUMP(status) || access(CORE_PATH, F_OK) == 0) {
        return fail("a core from a process that is not dumpable");
    }
    return 0;
}

int main(int argc, char **argv)
{
    struct stat st;
    int status;
    pid_t child;

    first_arg = argc > 0 ? (uintptr_t)argv[0] : 0;

    unlink(CORE_PATH);
    unlink(TARGET_PATH);
    if ((mkdir(DIR_PATH, 0700) != 0 && errno != EEXIST) || symlink(TARGET_PATH, CORE_PATH) != 0) {
        return fail("making the directory and the link");
    }
    child = abort_child(RLIM_INFINITY, 1, &status);
    if (child < 0 || !WIFSIGNALED(status) || WTERMSIG(status) != SIGABRT || !WCOREDUMP(status)) {
        return fail("an abort with no core flag");
    }
    if (lstat(CORE_PATH, &st) != 0 || !S_ISREG(st.st_mode) || (st.st_mode & 07777) != 0600 ||
        access(TARGET_PATH, F_OK) == 0) {
        return fail("the file where the link was");
    }
    if (core_check(child) != 0) {
        return 1;
    }

    unlink(CORE_PATH);
    child = abort_child(8192, 1, &status);
    if (child < 0 || !WIFSIGNALED(status) || WCOREDUMP(status)) {
        return fail("a core past a two-page limit");
    }
    if (short_check(child, 8192) != 0) {
        return 1;
    }
    unlink(CORE_PATH);
    if (no_core_rows() != 0 || hole_row() != 0) {
        return 1;
    }
    rmdir(DIR_PATH);
    printf("COREMUSL OK\n");
    return 0;
}
