# Advisory locks

`lock.c` keeps the advisory file locks of the three kinds Linux has, POSIX record locks, open file description (OFD) locks and `flock(2)` locks, and `lock.h` is its interface. A lock stops another lock and never a read or a write. The module exists because programs written for Linux share files by these locks and depend on their conflicts, their waits and the closes that drop them.

## Interfaces

The system calls are `fcntl(2)`'s lock commands, `F_GETLK`, `F_SETLK` and `F_SETLKW`, their 64-bit forms and the three `F_OFD_` commands (`fcntl_lock` in `../syscall_fs.c`), and `flock(2)` (`sys_flock`). [fcntl(2)](../../../man/man2/fcntl.2) and [flock(2)](../../../man/man2/flock.2) give the contract: the owners, the conflicts, which closes drop which locks, the errors, and the structures on each architecture. [proc(5)](../../../man/man5/proc.5) gives `/proc/locks`.

The rest of the kernel calls `lock_record` and `lock_flock` for those calls; `lock_close` at the close of a descriptor of the calling thread's table (`fd_close_locks` in `../syscall_fs.c`, for `close`, `dup2` over an open descriptor, the close-on-exec sweep and the release of a table by its last sharer, and not for an `O_PATH` descriptor or the copy of an entry a mapping or a message in flight keeps); `lock_ofd_gone` at a description's last reference (`fd_ofd_unref` in `fd.c`); `lock_repoint` when KRFFS moves a node (`vfs_repoint`); `lock_any`, which a close tests first; and `lock_count` and `lock_rows` for `/proc/locks`.

## Design

### Locks and owners

The locks are one list from the heap, `locks`, newest first. A lock (`struct file_lock`) has its key, its kind (`LK_POSIX`, `LK_OFD` or `LK_FLOCK`), its owner, the thread group id of the process that took it, its type, and its first and last byte, the last being `LOCK_EOF` for a lock to the end of the file however far the file grows. The owner is a pointer used as a number: the descriptor table, `current_proc->fds`, for a POSIX lock, so the threads of a process and the processes `clone` makes with `CLONE_FILES` are one owner, as a Linux `files_struct` is; and the description for the other two kinds. Two locks conflict when they have one key, are of different owners or kinds, overlap, and one is a write lock; `flock` locks meet only `flock` locks.

### The key

A lock's file is a `struct lock_key`: the mount, and a number that tells the file from every other while a description of it is open. `fd_lock_key` in `../syscall_fs.c` makes it from the description alone, so a close finds it without reading the volume, even on a mount whose device is gone. The mount is the base mount (`vfs_mount_base`), so a file has the same locks through a bind.

On KRFFS, the boot root and a volume, and on FAT32 and exFAT, the number is the description's node (`vnode`): the node's address on the root, its offset plus one on a volume, and the driver's record of the held file on the FAT types. Their inode numbers do not stay with the file: a file's on the KRFFS root mixes its directory's id with the hash of its name, which a rename changes and two names can share (`bC` and `cb` do), a volume's is the node's offset, and the FAT types' is the directory entry. On every other type the number is the inode number the description recorded at the open (`ino`). A descriptor that is not on a file of a mounted type, a pipe, a socket, a device or a terminal, is keyed by the object it is open on, with no mount, so two opens of one device share its locks. The third field, `shown`, is the recorded inode number; it takes no part in matching and is what `/proc/locks` prints, so `lslocks` finds the file as `stat` gives it.

A KRFFS write that grows a file past its extent can move its node. `vfs_repoint` moves every holder of the node, and `lock_repoint` moves the locks and the key of every wait for one, so a lock taken before the move meets one taken after it and a file later made at the old place meets neither. For the same reason the KRFFS root drops a description's locks before it frees an orphan the description held last (`fd_free_orphan`), since the description's own end comes after the free.

### Setting and waiting

`record_apply` sets or removes a record lock. It counts the records it needs, one for the new lock and one more to split a lock of the owner the request falls inside, and takes them from the heap before it changes anything, so `ENOLCK` leaves the locks as they were. It then cuts back, splits or removes the owner's locks over the range, puts the new one first, and joins to it the owner's locks of its type that touch it (`merge`), so `F_GETLK` reports one range where one was taken.

Every wait sleeps on one queue, `lock_wq`, which every change that can end a wait wakes, and each waiter then tests its own condition again. A record wait keeps two records on its stack: a `struct lock_wait`, with its owner and the owner of the lock it waits for, and a `struct lock_waiting`, the copy of its key that `lock_repoint` moves. Before each sleep `record_ready` finds the conflicting lock as it is now, records its owner, and follows the chain of waits from there (`deadlock`); a chain that comes back to the waiter's owner within `DEADLOCK_HOPS`, ten, closes a cycle, and the wait fails with `EDEADLK`. Of an owner waiting in two threads the chain follows the first wait it finds, as Linux's does. Because the test runs at every wake, a waiter behind two read locks waits for the second when the first goes, and a cycle is found by whichever wait closes it.

`lock_flock` looks for the description's own lock first: one of the type asked for is kept, and one of the other type is let go before the wait. The look is made again after every wake, so threads that share a description and wait together end with one lock. The new lock is taken from the heap last.

A set-lock command may gain its lock after another thread sharing the table closed the descriptor. That close dropped the table's locks on the file and left the descriptor closing ([Readme.md](Readme.md)); `fcntl_lock` finds `FD_FLAG_CLOSING` set, makes the key again, since a KRFFS file may have moved during the wait, drops the table's locks on the file and answers `EBADF`.

The list has no lock of its own: every system call runs under the kernel's one big lock (`../proc/smp.h`), and the module sleeps only in its waits.

## Limits

* There are no mandatory locks (`flock`'s `LOCK_MAND` is accepted and ignored) and no leases (`F_SETLEASE` and `F_GETLEASE` answer `EINVAL`).
* The deadlock search follows ten waits and covers POSIX locks only, so two processes that take exclusive `flock` or OFD locks on two files in opposite orders can wait for each other for good, as on Linux.
* A `flock` conversion is not atomic, and one that fails with `ENOLCK` leaves the description with no lock.
* Every lock operation walks the whole list, and so does a close while any lock exists; with none, a close costs one test.
* `/proc/locks` lists the locks held and not the waits.

## Tests

* `tests/run host` runs `lock_hosttest`, which includes `lock.c` with the wait queue and the heap stood in by the host: conflicts, `F_GETLK`, splits, joins, locks to the end of the file, the closes that drop a table's locks, shared tables as one owner, OFD and `flock` locks, conversions, cycles, and locks and waits that move with their file. `open_hosttest` checks the key `fd_lock_key` makes for a file on the KRFFS root, on a KRFFS volume and on another type, and requires an orphan's last description to drop its locks before the root frees the node.
* `tests/run default` runs `musl/lockmusl`: the three kinds between processes, waits and `EDEADLK`, a wait whose descriptor another thread closes, a table shared under `CLONE_FILES`, names that hash alike, locks across a rename and a move, and `/proc/locks`.
