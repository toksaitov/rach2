# Advisory locks

`lock.c` keeps the advisory file locks of the three kinds Linux has, and `lock.h` is its interface. The system calls that reach it are `fcntl(2)`'s lock commands and `flock(2)`, in `../syscall_fs.c`; the descriptor layer (`fd.c`) and the close path tell it when a lock's owner goes. A lock is advisory: it stops another lock, and never a read or a write.

## The kinds

A POSIX record lock (`F_SETLK`, `F_SETLKW`, `F_GETLK` and their 64-bit forms) covers a range of bytes and belongs to a process, whose threads are one owner, known by the thread group id. A second lock of the same process over the same bytes replaces the first, of either type, splitting or cutting back what it covers, and locks of one owner and one type that touch are joined, so `F_GETLK` reports one range where one was taken. Every close by the process of a descriptor of the file drops all its record locks on the file, whichever descriptor took them, as POSIX has it; the close of an `O_PATH` descriptor drops none, as on Linux. A lock is not inherited by `fork` and is kept by `execve`.

An open file description lock (`F_OFD_SETLK`, `F_OFD_SETLKW`, `F_OFD_GETLK`) is a record lock that belongs to the open file description instead: every descriptor a `dup` or a `fork` made shares it, a close drops it only with the description's last reference, and a second open of the file in the same process is another owner.

A `flock(2)` lock covers the whole file and belongs to the description, as an OFD lock does. flock locks and record locks do not meet: each kind conflicts only with its own, as on Linux.

Two owners' locks conflict when their ranges overlap and one of them is a write lock (`F_WRLCK`, or `LOCK_EX` for flock). A request that conflicts fails with `EAGAIN` (`EWOULDBLOCK` for flock with `LOCK_NB`) or waits, with `F_SETLKW`, `F_OFD_SETLKW` or a flock without `LOCK_NB`, until the conflict is gone. A wait is ended by a signal with `EINTR`, which a handler with `SA_RESTART` restarts. A flock conversion from one type to the other lets go of the old lock first, so it is not atomic, as on Linux.

## The key

A lock is on a file, found by the file's mount and inode number (`struct lock_key`). The inode number is the one the file had when the description was opened, which the description keeps (`ino` in `struct open_file`), so a close and a lock find the key without reading the volume, even on a mount whose device is gone. Two descriptions of one file therefore share a key when the file's inode number did not change between their opens. It does change on KRFFS: the boot root numbers a file by its directory and its name, so a rename gives it a new number, and a mounted volume numbers a file by its offset, which a move changes. A lock taken before such a change does not meet one taken through a description opened after it.

A descriptor that is not a file of a mounted type, a pipe, a socket, a device or a terminal, is locked by the object it is open on, with no mount, so two opens of one device share its locks as two opens of one file do.

## Deadlock

A wait for a POSIX record lock that would close a cycle of waiting processes fails with `EDEADLK`. The process that waits records whose lock it waits for, and before a wait the chain of those records is followed from the lock's owner; a chain that comes back to the waiter is a cycle. The chain is followed ten steps at most, Linux's bound, so a longer cycle is not found. Waits for OFD and flock locks are not checked, as on Linux.

## Limits

The locks are a list from the heap, with no fixed count; a lock the heap cannot give fails with `ENOLCK`, before anything changes. Every lock operation walks the whole list, and so does the close of a descriptor whenever any lock exists anywhere (`lock_any`); a close with no lock in the system costs one test.

## Verification

`tests/host/lock_hosttest.c` runs the lock rules on the host: a record lock that blocks another owner's over any byte it covers, `F_GETLK`'s report, an unlock that splits a lock, replacement and joining, a lock to the end of the file, a close that drops the closing process's locks on one file only, OFD locks against each other and against a record lock, flock locks against each other and apart from record locks, a conversion, and the end of a description. `tests/musl/lockmusl.c`, in the default suite, runs them between processes on Rach2: a child's lock refusing the parent's, `F_GETLK` naming the child, a split, a close of another descriptor dropping the child's locks, `F_SETLKW` waiting for the holder, one `EDEADLK` for two processes that wait for each other, flock's sharing through `dup` and release at the last close, and an OFD lock conflicting with a second open and with the same process's POSIX lock.
