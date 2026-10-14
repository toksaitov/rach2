#ifndef _COREDUMP_H_
#define _COREDUMP_H_

#include "../lib/kint.h"

/*
 * The core file of a process a signal ends, as Linux's `elf_core_dump`
 * writes it: an ELF file of type ET_CORE, of the class the architecture's
 * programs have, whose PT_NOTE segment has the threads' registers and the
 * process's state, and whose PT_LOAD segments have the memory of its
 * regions, which GDB reads. See Readme.md.
 */

/** Where the file goes: write `len` bytes from `buf` at the end of what was
 * written, or `len` zeros for a null `buf`, which the writer may leave as a
 * hole; 0, or a negative errno. */
typedef int32_t (*core_write_fn)(void *ctx, const void *buf, uint32_t len);

/**
 * Build the PT_NOTE segment of the current process's core file in the heap,
 * with `signo` the signal that ends it: the threads' registers and
 * floating-point state. It first waits until every other thread of the
 * process, which the caller has sent SIGKILL with the group's `core_dumping`
 * set, has parked (`core_park`) or ended, as Linux's `coredump_wait` waits
 * for them, so that none runs in user mode on another processor while its
 * registers are copied or its memory written; an execve of one of them that
 * waits for this thread to end ends the wait instead. `*size` gets its
 * bytes; NULL when the heap has no room or the size would not fit in 32 bits.
 */
uint8_t *core_notes(int signo, uint32_t *size);

/**
 * Park the current thread, which a SIGKILL ends while another thread of its
 * process writes the core: its floating-point state saved to its record and
 * its user registers on its kernel stack, where `core_notes` reads them, until
 * `core_release`. The caller has released nothing yet, so its memory and
 * descriptors are as the dump finds them.
 */
void core_park(void);

/** Let the parked threads of the current process go on to their end, once
 * its core is written or given up. */
void core_release(void);

/** The bytes a note of `descsz` bytes takes, its name "CORE" or "LINUX"
 * padded to eight bytes and its description to a word. */
uint32_t core_note_bytes(uint32_t descsz);

/** Put the note `type` named `name` with the `descsz` bytes of `desc` at
 * `*p` and step past it. */
void core_note_put(uint8_t **p, const char *name, uint32_t type, const void *desc, uint32_t descsz);

/**
 * Write the core file of the current process through `write` with `ctx`,
 * with the notes `core_notes` built: at most `limit` bytes, the soft
 * RLIMIT_CORE, past which the file stops short, as Linux's does. A page of
 * a file the process never touched is read from the file; the zeros of a
 * page of anonymous memory it never touched are a hole, which does not
 * count toward the limit. 0, or a negative errno: -EFBIG at the limit,
 * -ENOMEM, or the writer's.
 *
 * The other threads are parked or ended, since `core_notes` waited for them.
 * The regions are taken once, and the program headers and the pages follow
 * that list, since another process that shares the address space, as a vfork
 * child does, may still change its regions while the writer sleeps.
 */
int32_t core_write(core_write_fn write, void *ctx, const uint8_t *notes, uint32_t notes_size,
                   uint64_t limit);

/**
 * Write the core file of the current process through `core_write` as
 * "core" in its working directory, a new file of mode 0600 less the umask,
 * after unlinking whatever had the name, at most `limit` bytes, the soft
 * RLIMIT_CORE. 0, or a negative errno. Defined in syscall_fs.c, which opens
 * files.
 */
int32_t fs_core_file(int signo, unsigned long limit);

#endif /* _COREDUMP_H_ */
