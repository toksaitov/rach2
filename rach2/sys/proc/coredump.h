#ifndef _COREDUMP_H_
#define _COREDUMP_H_

#include "../lib/kint.h"

/*
 * The core file of a process a signal ends, as Linux's `elf_core_dump`
 * writes it on i386: an ELF32 file of type ET_CORE whose PT_NOTE segment has
 * the threads' registers and the process's state, and whose PT_LOAD segments
 * have the memory of its regions, which GDB reads. See ptrace.Readme.md.
 */

/** Where the file goes: write `len` bytes from `buf` at the end of what was
 * written, or `len` zeros for a null `buf`, which the writer may leave as a
 * hole; 0, or a negative errno. */
typedef int32_t (*core_write_fn)(void *ctx, const void *buf, uint32_t len);

/**
 * Build the PT_NOTE segment of the current process's core file in the heap,
 * with `signo` the signal that ends it: the threads' registers as they are
 * now, which is why it is built before the file is opened, since the other
 * threads may run and end while the file system waits, and an ended thread's
 * record is freed. `*size` gets its bytes; NULL when the heap has no room.
 */
uint8_t *core_notes(int signo, uint32_t *size);

/**
 * Write the core file of the current process through `write` with `ctx`,
 * with the notes `core_notes` built: at most `limit` bytes, the soft
 * RLIMIT_CORE, past which the file stops short, as Linux's does. A page of
 * a file the process never touched is read from the file; the zeros of a
 * page of anonymous memory it never touched are a hole, which does not
 * count toward the limit. 0, or a negative errno: -EFBIG at the limit, -ENOMEM, or the
 * writer's.
 *
 * It first waits until the process's other threads, which the caller has
 * sent SIGKILL, have ended, as Linux's `coredump_wait` waits for them, so
 * none of them is left in a call that changes the memory while it is
 * written; an execve of one of them that waits for this thread to end ends
 * the wait instead. The regions are then taken once, and the program
 * headers and the pages follow that list, since another process that shares
 * the address space, as a vfork child does, may still change its regions
 * while the writer sleeps.
 */
int32_t core_write(core_write_fn write, void *ctx, const uint8_t *notes, uint32_t notes_size,
                   uint32_t limit);

/**
 * Write the core file of the current process through `core_write` as
 * "core" in its working directory, a new file of mode 0600 less the umask,
 * after unlinking whatever had the name. 0, or a negative errno. Defined in
 * syscall_fs.c, which opens files.
 */
int32_t fs_core_file(int signo, uint32_t limit);

#endif /* _COREDUMP_H_ */
