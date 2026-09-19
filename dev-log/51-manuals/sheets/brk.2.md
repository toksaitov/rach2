# brk(2)

NAME: brk - the system call family: brk, mmap2, munmap.

The page names every member in NAME with .Nm, gives each its prototype in SYNOPSIS as musl declares it (the header and the C prototype; the number in the table is the ABI, EAX), describes the behaviour of this kernel with its differences from Linux where a caller would notice, lists every error the handler returns in ERRORS with when, and ties an example to a probe of the default suite where one exists (tests/*.c). Aliases: every member other than the page name is already listed in man/aliases as an alias of this page; touch no file but the page.

Notes from the milestone: The heap and the mappings: anonymous mappings only or file mappings; the flags honoured; page granularity; RLIMIT_AS; the mapping count limit (VMA_LIMIT); a fault on a missing frame is SIGSEGV.

Sources: the handler comments below, extracted from rach2/sys/syscall.c (read the handler and the helpers it calls for what the comment does not say); rach2/sys/fs/Readme.md, vfs.Readme.md, network/Readme.md; the kernel Readme.


## brk: number 45, handler sys_brk

sys_brk - set the program break (heap top). Arg: EBX = requested break.
Returns the resulting break: the new value on success, or the unchanged
current break if the request is out of range. This is the contract musl's
__brk relies on (it compares the return to its request). The heap is a
single demand-paged VMA [heap_start, page_up(brk)); growing only extends
the region (pages fault in lazily), shrinking frees the pages above.


## mmap2: number 192, handler sys_mmap2

sys_mmap2 - map anonymous memory. Args: EBX=addr (hint, ignored),
ECX=length, EDX=prot, ESI=flags, EDI=fd, EBP=pgoffset. Returns the mapped
base address, or -errno. Only MAP_ANONYMOUS|MAP_PRIVATE is supported (what
musl's allocator uses); the region is placed by bumping mmap_top down and
is demand-paged + zero-filled on touch.

### helper vma_exhausted

Say on the serial log that a process ran out of mappings, once per process.
`vm.c` is deliberately independent of the kernel's globals, so it cannot
name the process it refused; here the caller is known. Silence is what this
replaces, and it cost hours: a program whose library ran out of mappings
looks exactly like a program whose library failed for its own reasons. Said
once per process, because a program that meets the ceiling meets it in a
loop and a flood of lines would bury the first one. The mark is in the
process, not a remembered pid: a pid is reused, and the next process to hold
that number would be silenced by a report it never made.


## munmap: number 91, handler sys_munmap

sys_munmap - unmap a range. Args: EBX=addr (page-aligned), ECX=length.
Returns 0, or -errno. Drops the VMA bookkeeping (trimming or splitting
partial overlaps) and frees the backing frames; unmapping never-faulted or
never-mapped pages is a no-op, as on Linux.

### helper vma_exhausted

Say on the serial log that a process ran out of mappings, once per process.
`vm.c` is deliberately independent of the kernel's globals, so it cannot
name the process it refused; here the caller is known. Silence is what this
replaces, and it cost hours: a program whose library ran out of mappings
looks exactly like a program whose library failed for its own reasons. Said
once per process, because a program that meets the ceiling meets it in a
loop and a flood of lines would bury the first one. The mark is in the
process, not a remembered pid: a pid is reused, and the next process to hold
that number would be silenced by a report it never made.
