# syscalls(2)

NAME: syscalls - the system calls this kernel answers, by number. Source: rach2/sys/syscall.h (the numbers), the table in rach2/sys/syscall.c, dev-log/51-manuals/sheets/syscalls.md (the list with handlers).

* The ABI: `int 0x80`, the number in EAX, arguments in EBX, ECX, EDX, ESI, EDI (and EBP for a sixth), the result in EAX, a negative errno for a failure; the Linux i386 numbers, so musl's system call layer is used unchanged; a number the table does not hold answers ENOSYS.
* A table of every registered number and name, each `.Xr NAME 2` pointing at its page (the family page or its alias): the 120 calls and the socketcall operations with their operation numbers.
* What is not there that Linux has: threads (clone), ptrace, the *at forms not listed, sendmsg, socketpair, accept4, epoll, inotify, mprotect, madvise (state the ENOSYS rule rather than a list).
* SEE ALSO: intro to section 2 is this page; socketcall(2); rach2(7).
