/**
 * Core files, made in /dev/shm, a tmpfs that keeps the holes of a file.
 *
 * A child that raises its soft RLIMIT_CORE and aborts leaves "core" in its
 * working directory, where a symbolic link had the name: the parent's wait
 * reports SIGABRT with WCOREDUMP, the link's target is never created, and
 * the file is an i386 ELF core whose NT_PRSTATUS names the child and
 * SIGABRT, whose NT_PRPSINFO has the program's name, whose NT_AUXV has the
 * page size, and whose PT_LOAD for the child's data has the word the child
 * stored there. A child whose soft limit is two pages leaves no WCOREDUMP
 * and a file of its own, with the child's notes, that stops before its last
 * segment ends and has no more than two pages of bytes that are not zero,
 * since only a hole reads as zeros without being written. One whose limit
 * is 0, and one that is not dumpable, leave no file and no WCOREDUMP.
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
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define DIR_PATH    "/dev/shm/coremusl.d"
#define CORE_PATH   DIR_PATH "/core"
#define TARGET_PATH DIR_PATH "/target"
#define MARK        0x5ca1ab1eu

static volatile uint32_t marker;

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
static int notes_check(const unsigned char *core, const Elf32_Phdr *seg, pid_t child)
{
    const unsigned char *p = core + seg->p_offset;
    const unsigned char *end = p + seg->p_filesz;
    int status = 0, info = 0, auxv = 0;

    while (p + 12 <= end) {
        const Elf32_Nhdr *n = (const Elf32_Nhdr *)p;
        const unsigned char *desc = p + 12 + ((n->n_namesz + 3) & ~3u);

        if (n->n_type == NT_PRSTATUS && n->n_descsz == 144 && !status) {
            int16_t cursig;
            int32_t pid;

            memcpy(&cursig, desc + 12, sizeof(cursig));
            memcpy(&pid, desc + 24, sizeof(pid));
            status = cursig == SIGABRT && pid == child;
        } else if (n->n_type == NT_PRPSINFO && n->n_descsz == 124) {
            info = strncmp((const char *)desc + 28, "coremusl", 16) == 0;
        } else if (n->n_type == NT_AUXV) {
            const uint32_t *a = (const uint32_t *)desc;
            uint32_t i;

            for (i = 0; i + 1 < n->n_descsz / 4 && a[i] != AT_NULL; i += 2) {
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

/** Check the core file of `child`: an ELF core for i386 whose notes are
 * right and whose memory has the mark at the marker's address. */
static int core_check(pid_t child)
{
    size_t size;
    unsigned char *core = slurp(CORE_PATH, &size);
    const Elf32_Ehdr *eh = (const Elf32_Ehdr *)core;
    uintptr_t at = (uintptr_t)&marker;
    int notes = 0, found = 0;
    int i;

    if (!core || size < sizeof(*eh)) {
        return fail("reading the core file");
    }
    errno = 0;
    if (memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0 || eh->e_ident[EI_CLASS] != ELFCLASS32 ||
        eh->e_type != ET_CORE || eh->e_machine != EM_386 || eh->e_phentsize != sizeof(Elf32_Phdr) ||
        eh->e_phoff + (size_t)eh->e_phnum * sizeof(Elf32_Phdr) > size) {
        return fail("the ELF header");
    }
    for (i = 0; i < eh->e_phnum; i++) {
        const Elf32_Phdr *ph = (const Elf32_Phdr *)(core + eh->e_phoff) + i;

        if ((size_t)ph->p_offset + ph->p_filesz > size) {
            return fail("a segment past the end of the file");
        }
        if (ph->p_type == PT_NOTE) {
            if (notes++ != 0 || notes_check(core, ph, child) != 0) {
                return notes > 1 ? fail("two note segments") : 1;
            }
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
    const Elf32_Ehdr *eh = (const Elf32_Ehdr *)core;
    int notes = 0;
    int k;

    if (!core || size < sizeof(*eh)) {
        return fail("reading the short core file");
    }
    errno = 0;
    if (memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0 || eh->e_type != ET_CORE ||
        eh->e_phoff + (size_t)eh->e_phnum * sizeof(Elf32_Phdr) > size) {
        return fail("the short core's ELF header");
    }
    for (k = 0; k < eh->e_phnum; k++) {
        const Elf32_Phdr *ph = (const Elf32_Phdr *)(core + eh->e_phoff) + k;

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

int main(void)
{
    struct stat st;
    int status;
    pid_t child;

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
    child = abort_child(0, 1, &status);
    if (child < 0 || !WIFSIGNALED(status) || WCOREDUMP(status) || access(CORE_PATH, F_OK) == 0) {
        return fail("a core with the limit 0");
    }
    child = abort_child(RLIM_INFINITY, 0, &status);
    if (child < 0 || !WIFSIGNALED(status) || WCOREDUMP(status) || access(CORE_PATH, F_OK) == 0) {
        return fail("a core from a process that is not dumpable");
    }
    rmdir(DIR_PATH);
    printf("COREMUSL OK\n");
    return 0;
}
