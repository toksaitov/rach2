# The system calls, as the handlers describe themselves

Generated from rach2/sys/syscall.c by the milestone: each registered call, its number, its handler, the handler's comment, and the comments of up to three helpers it calls that have a documentation comment. The section 2 sheets point here; the page writer reads the code for the rest.


## 1 exit -> sys_exit

Terminate the current process (exit(2)/exit_group(2)). Does not return.
EBX has the exit status. Releases files and cwd, reparents children to init,
publishes the zombie and wakes its parent, then reschedules. A normal child
leaves its mappings, descriptor-table storage, record and stack to do_wait.
A real init's exit is fatal (panic_init_died); an init exiting under the
debug shell returns to it.
The plain status line goes out only where the debug shell is the supervisor.
There it is how a person, and the harness in its no-token mode, read a
transient init's outcome. On an image running `/sbin/init` and a login shell
every process exit would print it, a forked child's included, and the shell
already answers the question through `$?`. The line that hands the shell back
below is not gated: the probe runner scores probes from that one.
An init exiting under the debug shell first ends every other user process
through `proc_end_others`, then returns without scheduling: a child left
runnable would take the processor, and the exited init, a zombie, would
never be picked again to make the return. A process that is not init
leaves through the scheduler as a zombie its parent reaps; when nothing is
ready at that moment, since its parent may be gone, it idles for the next
interrupt with interrupts off between checks and schedules on its own,
because the timer's preemption is deferred inside a call. A halt there
kept the machine forever.

### helper proc_end_others

End every other user process before init returns the machine to the shell.
What the program left running is killed and reaped here while init is
still a live process the scheduler runs: a leftover asleep in the disk
driver with a mount's lock held finishes its call and releases the lock
before the signal ends it, and a leftover spinning in user mode takes the
signal at its next interrupt, since signals are delivered on the return
from an interrupt as well. The shell's resume disables the scheduler, so
anything left runnable or holding a lock then would run no further, and
the next program would wait on it forever. A zombie is reaped as `wait`
reaps one. Kernel threads and the idle record are not user processes
and stay.


## 2 fork -> sys_fork

Create a child process (fork(2)).
Clones the current process: PCB, page directory (copy-on-write user
pages), and fd table. The child is set up to return from fork with value
0; the parent gets the child's PID. Returns -errno on failure.
Among what the child inherits is the program name, which `execve` sets and a
child that never execs would otherwise never have: a daemon that forks
itself into the background is exactly such a child, and without this it
shows in `ps` and in /proc/<pid>/stat with no name for as long as it runs.

### helper kstack_free

Give back the stack whose top is `top`. */


## 3 read -> sys_read

read(2): EBX = fd, ECX = buf, EDX = count. */

### helper read_fd

Read up to `count` bytes from descriptor `fd` (read(2)).
EBX = fd, ECX = buf, EDX = count. Returns the bytes read, 0 at end, or
-errno. Dispatches by backing kind. A console read (stdin) blocks in the
tty line discipline until a cooked line or VMIN raw bytes arrive; in
graphics mode it is a non-blocking raw scancode read instead.


## 4 write -> sys_write

write(2): EBX = fd, ECX = buf, EDX = count. */

### helper write_fd

Write `count` bytes of user buffer `buf` to descriptor `fd`.
The shared body of write(2) and writev(2). Dispatches by backing kind: an
ops-backed object (pipe/tty/pty/device) through its vtable, a socket to the
network, the console through the tty output processor, a KRFFS file, or the
framebuffer. Returns the bytes written or a negative errno.


## 5 open -> sys_open

(no comment)


## 6 close -> sys_close

close(2): EBX = fd. Returns 0 or -EBADF. */

### helper fd_release_backing

Release every backing reference a descriptor holds before its slot is freed.
Drops framebuffer ownership, the per-fd socket reference, and an ops-backed
object (pipe/tty/pty). Every fd-teardown site routes through this (close, the
dup2 replacement of an open target, the execve CLOEXEC sweep, and the
process-exit fd-table release) so none can forget a backing kind as new ones
are added. It does NOT free the slot; the caller does that with fd_free.
An ops-backed object rides the shared open-file description: dup/dup2/fork
copy the ops/priv pointers and bump the OFD refcount, so the object must
be released exactly once, when its LAST descriptor closes (the final OFD
reference, refcount about to reach 0). The socket reference, by contrast,
is counted per descriptor (socket_ref on dup/fork), so it is always
dropped here. No descriptor is both an ops object and a socket.


## 7 waitpid -> sys_waitpid

sys_waitpid - wait for a child to change state.
Args: EBX = pid (-1 = any child), ECX = status pointer, EDX = options.
Returns the reaped child's PID, 0 (WNOHANG, none ready), or -errno.

### helper do_wait

Shared core of waitpid(7) and wait4(114).
`status` (may be NULL) receives the Linux-encoded wait status that
sys_exit/terminate already stored in exit_code; `rusage_ptr` (0 for
waitpid, may be 0 for wait4) is zero-filled on reap. With WNOHANG, returns
0 instead of blocking when a child exists but has not exited.


## 8 creat -> sys_creat

creat(2): EBX = path, ECX = mode. Returns an fd open for writing, or
-errno. An existing regular file is truncated to zero by the call; a
directory, the root included, is EISDIR.


## 9 link -> sys_link

(no comment)


## 10 unlink -> sys_unlink

unlink(2): EBX = path. Returns 0 or -errno.
Every refusal is decided here, not below, because `krffs_unlink`
answers all of them with a bare -1 that userspace decodes as `EPERM`
whatever went wrong. A missing component gives `-ENOENT` and a plain file
in the middle of the path `-ENOTDIR`, both from the reporting resolver;
the root and any directory give `-EISDIR`, which is what tells a caller
to use `rmdir`. The root is tested by node identity because it has
0755 with no type bits and would otherwise read as a plain file.

### helper cred_remove_ref

The removal rules for a directory on a type other than the root: write
permission on the directory, and in a sticky directory the entry's owner,
the directory's owner, or root. The root type keeps its own copy of the
rule beside its node fields.

### helper cred_access_node

The permission check over a KRFFS node. The root directory reads as 0755,
the fixed identity stat reports (its stored mode has the dirty-mount
flag). Executing a regular file demands an execute bit somewhere even for
root: root runs programs, not data.

### helper cred_sticky_ok

The sticky rule on removing or renaming `victim` out of directory `dir`.
With the directory's sticky bit set, write permission on it is no longer
enough: only the entry's owner, the directory's owner, or a privileged caller
may take an entry out. That is what makes a shared 1777 directory usable, and
`-EPERM` is the errno POSIX names for it, not `EACCES`.
The SVR4 relaxation that also permits it when the caller can write the file
is not POSIX and is not adopted here: it would let a mode a victim chose for
their own file decide who may delete it.
The root is non-sticky by node identity, not by reading its mode, the
same fixed-mode rule `cred_access_node` uses, so `KRFFS_MODE_DIRTY` never
enters a permission answer.


## 11 execve -> sys_execve

Replace the current process image with a new program (execve(2)).
EBX = path, ECX = argv[], EDX = envp[]. Does not return on success. Copies
the path, argv, and envp into the process's exec workspace, allocated at
its first exec, and validates the ELF header BEFORE tearing the old image
down (so an executable that is not an ELF leaves the caller intact), then frees the old user pages, streams the new program in, builds
the SysV/i386 initial stack, and redirects the iret frame to the entry
point. A failure after the old image is gone routes to
exec_fail_destroyed.

### helper cred_access_ref

The permission check over a node of any type, from the attributes its
file system reports: the same nine-bit rule as cred_access_node, plus
the execute rule for a regular file.

### helper cred_access_node

The permission check over a KRFFS node. The root directory reads as 0755,
the fixed identity stat reports (its stored mode has the dirty-mount
flag). Executing a regular file demands an execute bit somewhere even for
root: root runs programs, not data.

### helper exec_shebang

Rewrite an exec of a script into an exec of its interpreter.
A file whose first two bytes are `#!` names the program that should run it,
and Linux's rule is what everything expects: the rest of that line is the
interpreter and at most one argument, and the program then runs as
`interpreter [argument] script original-arguments...`. Without this, an exec
of a script fails with `ENOEXEC` and only a shell, which reads the file
itself, can run one. A DHCP client execs its lease script directly, so the
machine needs it.
One level only. A script whose interpreter is itself a script is refused,
not followed, because a chain of them is a loop nobody can see the
end of, and nothing here needs one.
The caller's `path` and `argv_bufs` are rewritten in place on success.
The script is read through `read` over `ctx`: the root's reader by path,
or the mount's over an open handle.
@param argc the argument count, updated when the vector grows.
@return 1 when rewritten, 0 when the file is not a script, or a negative
        errno for one whose first line names nothing.


## 12 chdir -> sys_chdir

Change the current working directory (chdir(2)).
EBX = path. Returns 0 or -errno. Stores the canonical absolute path of the
resolved directory (a KRFFS directory, a directory of another mounted
file system, or a synthetic /dev or /proc directory); a regular file is
-ENOTDIR, a missing path -ENOENT.

### helper cred_access_ref

The permission check over a node of any type, from the attributes its
file system reports: the same nine-bit rule as cred_access_node, plus
the execute rule for a regular file.

### helper collapse_dots

Collapse `.` and `..` in an absolute path that a physical walk has just
resolved, for the working directory's string: after the walk succeeded,
every `..` names the parent the walk climbed to. `path` must start with
'/'; the output never grows, so the rewrite is in place.

### helper cred_access_node

The permission check over a KRFFS node. The root directory reads as 0755,
the fixed identity stat reports (its stored mode has the dirty-mount
flag). Executing a regular file demands an execute bit somewhere even for
root: root runs programs, not data.


## 15 chmod -> sys_chmod

chmod(path, mode) and fchmodat(dirfd, path, mode[, flags]) share do_chmod. */

### helper do_chmod

chmod core for the chmod/chown family.
A KRFFS path stores through `krffs_chmod_node` under the owner-or-root rule.
A pty slave (`/dev/pts/N`) stores on the pty, the state dropbear moves
before handing the terminal to the user. Every other synthetic /dev or
/proc object has fixed root-owned metadata: root's request succeeds
without a change, anyone else's is refused. `name` is absolute with the
leading '/' stripped, matching do_access.


## 16 lchown -> sys_chown

chown/lchown/fchownat share do_chown; both the 16-bit and *32 numbers route
here since musl issues the 16-bit chown on i386. */


## 19 lseek -> sys_lseek

(no comment)

### helper raw_seek_resolve

Resolve a seek on a raw disk window, in unsigned arithmetic throughout.
The obvious form adds the delta to the base as a signed 64-bit value and
then checks the result for negativity, which is undefined the moment the sum
leaves the signed range: a descriptor parked at the top of that range and
seeked forward by one has no defined result to test. So the base is unsigned,
the delta's magnitude is taken without ever negating it (negating the most
negative value is itself undefined), and both directions are bounds-checked
before the arithmetic, not after.
@param cur    the window's current offset.
@param size   the window's byte size, for SEEK_END.
@param delta  the requested displacement, which may be negative.
@param whence 0 SEEK_SET, 1 SEEK_CUR, 2 SEEK_END.
@param out    the resolved offset, untouched on failure.
@return 0, `-EINVAL` for a bad whence or a result before the start, or
        `-EOVERFLOW` for one past what an offset can hold.

### helper seek_add

Add a seek displacement to a file offset without overflowing.
The sum of an offset and a displacement leaves the range of the type
for offsets a caller may legally ask about, and signed overflow has no
value in C to check afterwards, so the bound is tested first and the
arithmetic done unsigned.
@return 0, `-EINVAL` when the result would be negative, or
`-EOVERFLOW` when it would pass the largest offset a file can have.


## 20 getpid -> sys_getpid

sys_getpid - return the calling process's PID (getpid(2)).


## 21 mount -> sys_mount

mount(2): EBX = source, ECX = target, EDX = type, ESI = flags, EDI = data.
Effective uid 0 is required. The strings are copied here; the order of
work and every other refusal are the VFS's (vfs_do_mount). `data` may be
NULL, which is an empty option string.

### helper cred_privileged

Non-zero when the caller has appropriate privilege (effective uid 0). */

### helper join_abs

Join base + '/' + rel into pathbuf as an absolute path.
`base` is "/" or "/a/b" with no trailing slash; `rellen` is k_strlen(rel).
Returns the length, or -ENAMETOOLONG if it would not fit.


## 29 pause -> sys_pause

sys_pause - sleep until a signal is delivered (pause(2)). Returns -EINTR
once a deliverable signal is pending (the signal then acts on the return
to user).


## 33 access -> sys_access

(no comment)

### helper do_access

access core: does the path exist and grant the requested mode?
R/W/X are checked against the object's mode bits by the caller's REAL ids
(the credential model access(2) uniquely uses), which is what makes
/proc/<pid>/stat reject W_OK while granting F_OK/R_OK. `name` is absolute
with the leading '/' stripped.


## 36 sync -> sys_sync

Flush all buffered file-system writes to stable storage (sync(2)).
Takes no argument. Every active mount's `sync` runs and the buffer cache
writes its dirty buffers, then every dirty page of the disk-backed window
goes back via `pc_sync` (a no-op when KRFFS is the RAM module). Like
Linux's sync(2), it returns 0 even if a writeback failed; what failed
stays dirty for a later call, and a mount whose device failed a write is
read-only from then on.


## 37 kill -> sys_kill

(no comment)


## 38 rename -> sys_rename

rename(2): EBX = old path, ECX = new path. Returns 0 or -errno.
As in `unlink`, every refusal is decided here, because `krffs_rename`
answers all of them with a bare -1 that userspace reads as `EPERM`. The
two directory cases are the ones POSIX separates and this file system
does not: a directory SOURCE is `-EPERM`, since a directory's children
reference its id and a move would orphan them, so the rename cannot be
performed at all; a directory DESTINATION under a plain source is
`-EISDIR`, the case that errno is reserved for. The rest follow the
reporting resolver: `-ENOENT` for a missing source or destination parent,
`-ENOTDIR` for a plain file used as a path component, `-EINVAL` for a
destination naming no entry.
Either path naming the root is `-EINVAL`, from the empty-name check above:
`copy_path_cwd` canonicalizes it to "/", which loses its only component.
`-ENOENT` would be a lie, since the root exists, and POSIX names `EINVAL`
for a pathname that is the root. The identity tests below are therefore
unreachable today and are kept as defence: freeing the root as a replaced
destination would take the file system with it, and that guarantee should
not rest on a canonicalizer in another function.

### helper cred_access_node

The permission check over a KRFFS node. The root directory reads as 0755,
the fixed identity stat reports (its stored mode has the dirty-mount
flag). Executing a regular file demands an execute bit somewhere even for
root: root runs programs, not data.

### helper cred_access_ref

The permission check over a node of any type, from the attributes its
file system reports: the same nine-bit rule as cred_access_node, plus
the execute rule for a regular file.

### helper cred_remove_ref

The removal rules for a directory on a type other than the root: write
permission on the directory, and in a sticky directory the entry's owner,
the directory's owner, or root. The root type keeps its own copy of the
rule beside its node fields.


## 39 mkdir -> sys_mkdir

sys_mkdir - create a directory.
Args: EBX = path, ECX = mode (masked by umask, like any creation). Returns 0
or -errno (-EEXIST if it exists, -ENOENT if a parent is missing).

### helper cred_access_ref

The permission check over a node of any type, from the attributes its
file system reports: the same nine-bit rule as cred_access_node, plus
the execute rule for a regular file.

### helper cred_access_node

The permission check over a KRFFS node. The root directory reads as 0755,
the fixed identity stat reports (its stored mode has the dirty-mount
flag). Executing a regular file demands an execute bit somewhere even for
root: root runs programs, not data.

### helper krffs_stamp_new_node

Stamps a newly created KRFFS node with the creating process's identity:
owner and group from the effective ids, permission bits from `mode` with
the file-creation mask applied, the type bits kept. The V7 creation rule:
who you effectively are is whose the file is.


## 40 rmdir -> sys_rmdir

sys_rmdir - remove an empty directory.
Args: EBX = path. Returns 0 or -errno (-ENOENT, -ENOTDIR, -ENOTEMPTY).

### helper cred_remove_ref

The removal rules for a directory on a type other than the root: write
permission on the directory, and in a sticky directory the entry's owner,
the directory's owner, or root. The root type keeps its own copy of the
rule beside its node fields.

### helper cred_access_node

The permission check over a KRFFS node. The root directory reads as 0755,
the fixed identity stat reports (its stored mode has the dirty-mount
flag). Executing a regular file demands an execute bit somewhere even for
root: root runs programs, not data.

### helper cred_sticky_ok

The sticky rule on removing or renaming `victim` out of directory `dir`.
With the directory's sticky bit set, write permission on it is no longer
enough: only the entry's owner, the directory's owner, or a privileged caller
may take an entry out. That is what makes a shared 1777 directory usable, and
`-EPERM` is the errno POSIX names for it, not `EACCES`.
The SVR4 relaxation that also permits it when the caller can write the file
is not POSIX and is not adopted here: it would let a mode a victim chose for
their own file decide who may delete it.
The root is non-sticky by node identity, not by reading its mode, the
same fixed-mode rule `cred_access_node` uses, so `KRFFS_MODE_DIRTY` never
enters a permission answer.


## 41 dup -> sys_dup

Duplicate a descriptor onto the lowest free fd (dup(2)).
EBX = oldfd. Returns the new fd, or -errno. The duplicate SHARES oldfd's
open-file description (shared seek offset); a socket gains a reference;
the new fd's FD_CLOEXEC is clear.


## 42 pipe -> sys_pipe

(no comment)


## 43 times -> sys_times

Process times (times(2)).
Per-process CPU time is not tracked, so the four tms fields are reported
as zero; the return value is the monotonic tick count (already in CLK_TCK
= 100 Hz units). EBX = struct tms * (NULL is allowed: the caller may want
only the return value).


## 45 brk -> sys_brk

sys_brk - set the program break (heap top). Arg: EBX = requested break.
Returns the resulting break: the new value on success, or the unchanged
current break if the request is out of range. This is the contract musl's
allocator relies on when it issues SYS_brk (it compares the return to its
request; musl's public brk() never issues the call). The heap is a single
demand-paged VMA [heap_start, page_up(brk)); growing only extends the
region (pages fault in lazily), shrinking frees the pages above.


## 52 umount2 -> sys_umount2

umount2(2): EBX = target, ECX = flags. Effective uid 0 is required, and
the flags must be zero: MNT_FORCE, MNT_DETACH and UMOUNT_NOFOLLOW are
promises this kernel cannot keep and answer EINVAL.

### helper cred_privileged

Non-zero when the caller has appropriate privilege (effective uid 0). */

### helper join_abs

Join base + '/' + rel into pathbuf as an absolute path.
`base` is "/" or "/a/b" with no trailing slash; `rellen` is k_strlen(rel).
Returns the length, or -ENAMETOOLONG if it would not fit.


## 54 ioctl -> sys_ioctl

Device control (ioctl(2)).
EBX = fd, ECX = request, EDX = arg. A tty fd (console or pty slave) routes
to the shared termios/session handler first; an ops-backed object handles
its own remaining requests; the framebuffer and console handle their
device-specific ones (FB_*, KIOCSOUND, AUDIO_*). Returns 0, the request
result, or -errno (-ENOTTY for a terminal request on a non-terminal fd).

### helper tty_common_ioctl

The shared terminal ioctls operating on a GIVEN tty.
The real termios layer plus the session/job-control requests, so the
console and a pty slave share one implementation. Sets *handled = 1 and
returns the result for a recognized terminal request; sets *handled = 0
(return value unused) otherwise, so the caller falls through to its own
handling. TCGETS/TCSETS exchange the 36-byte kernel termios (n_tty.h);
TCSETSF and TCFLSH flush the unread input.


## 57 setpgid -> sys_setpgid

Set a process's process group (setpgid(2)).
EBX = pid (0 = caller), ECX = pgid (0 = the target's own pid, i.e. make it
a group leader). The target must be the caller or a child of the caller,
must not be a session leader, and the destination group must live in the
target's session. Returns 0 or -errno.


## 60 umask -> sys_umask

sys_umask - set the file-creation mask, returning the previous value
(umask(2)). Args: EBX = new mask (only the low 9 permission bits are
kept).


## 63 dup2 -> sys_dup2

dup2(2): EBX = oldfd, ECX = newfd. Returns newfd or -errno. */

### helper fd_release_backing

Release every backing reference a descriptor holds before its slot is freed.
Drops framebuffer ownership, the per-fd socket reference, and an ops-backed
object (pipe/tty/pty). Every fd-teardown site routes through this (close, the
dup2 replacement of an open target, the execve CLOEXEC sweep, and the
process-exit fd-table release) so none can forget a backing kind as new ones
are added. It does NOT free the slot; the caller does that with fd_free.
An ops-backed object rides the shared open-file description: dup/dup2/fork
copy the ops/priv pointers and bump the OFD refcount, so the object must
be released exactly once, when its LAST descriptor closes (the final OFD
reference, refcount about to reach 0). The socket reference, by contrast,
is counted per descriptor (socket_ref on dup/fork), so it is always
dropped here. No descriptor is both an ops object and a socket.


## 64 getppid -> sys_getppid

sys_getppid - return the parent's PID (getppid(2)).


## 65 getpgrp -> sys_getpgrp

sys_getpgrp - the caller's process-group id (getpgrp(2), the no-arg form).


## 66 setsid -> sys_setsid

Create a new session (setsid(2)).
The caller becomes the leader of a new session AND a new process group,
both numbered with its pid. Fails with -EPERM if the caller already leads
a process group (a group leader cannot carry its group into a new
session). Returns the new session id (the caller's pid).


## 74 sethostname -> sys_sethostname

sethostname(74): set the single mutable host name (uname's nodename, what
gethostname reads back). */


## 75 setrlimit -> sys_setrlimit

setrlimit(75): the 32-bit form of a set. */

### helper rlimit_set

Set a kept limit of `p`: the soft value at most the hard, the hard raised
by root only and lowered by anyone, and RLIMIT_NOFILE never above the
table's limit. A limit that is not kept is accepted and not changed.
0, or -EINVAL and -EPERM as POSIX names them.


## 76 getrlimit -> sys_getrlimit

getrlimit(76)/ugetrlimit(191): 32-bit struct rlimit { u32 rlim_cur,
rlim_max }. */


## 78 gettimeofday -> sys_gettimeofday

gettimeofday(2): EBX = tv (timeval*), ECX = tz (ignored). Reports real time
from the clock anchored at boot (sys/rtc.c), or time since boot when there
was no clock to read. Returns 0 or -errno.


## 82 select -> sys_select

(no comment)


## 83 symlink -> sys_symlink

(no comment)


## 85 readlink -> sys_readlink

(no comment)

### helper do_readlink

readlink core (readlink(2)/readlinkat(2)).
The links are /proc/self, the /proc/<pid>/fd/<n> magic links (which
ttyname reads), and the symbolic links of a mounted type that has them,
read through the type. The result is the byte count and is NOT
NUL-terminated (the readlink contract; musl appends its own terminator and
treats a full buffer as truncation). `name` is the path, leading '/'
stripped.


## 88 reboot -> sys_reboot

reboot(2): stop the machine. EBX = magic1, ECX = magic2, EDX = command.
Does not return for the three commands that stop the machine. Returns
`-EPERM` for an unprivileged caller, `-EINVAL` for a wrong magic or an
unknown command, and 0 for `RB_DISABLE_CAD`.
The privilege check comes before the magics, which inverts the order the rest
of this file uses, and Linux inverts it the same way. A magic-first handler
would let any uid learn the ABI of the one call that stops the machine and
erases the evidence of what it was doing; nothing is learned from `-EPERM`.
`RB_DISABLE_CAD` returning 0 is what makes the rest testable. Every command
that works stops the machine, so without one that succeeds quietly a caller
cannot tell an accepted magic from a rejected one: both would answer
`-EINVAL`. It asks for a state this kernel is permanently in, since there is
no keyboard reboot to disable, so 0 is a true answer. `RB_ENABLE_CAD` asks
for a feature that does not exist and answers `-EINVAL`, because a 0 there
would be the one reply a caller could not tell from a working one.

### helper cred_privileged

Non-zero when the caller has appropriate privilege (effective uid 0). */


## 91 munmap -> sys_munmap

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


## 93 ftruncate -> sys_ftruncate

(no comment)

### helper truncate_fd

Truncate an open file to `length` (ftruncate(2)).
The descriptor must be a writable file on a mounted file system. A
successful shrink or extend clears the set-id bits for an unprivileged
caller, like a write. `SYS_FTRUNCATE` takes the length in ECX, and
`SYS_FTRUNCATE64`, the form musl calls, in ECX and EDX as the low and
high halves; the file system answers a length past what it can hold.


## 94 fchmod -> sys_fchmod

fchmod(fd, mode): the descriptor form, under the rules do_chmod keeps:
a KRFFS node or a node of another mounted file system stores the mode, a
pty slave stores it on the pty, and every other descriptor has fixed
metadata (root succeeds without a change, anyone else is refused).

### helper krffs_chmod_node

The chmod rule over a resolved KRFFS node: the owner or root replaces the
permission bits; anyone else is refused. The root node's mode is never
touched (its spare bits carry the dirty-mount flag), but the rule still
answers, so a non-owner probing `/` gets the correct -EPERM.

### helper cred_privileged

Non-zero when the caller has appropriate privilege (effective uid 0). */

### helper cred_in_group

Non-zero when `gid` is the caller's effective gid or in its supplementary
list: the set a file owner may chgrp to, and the group the access check
matches. */


## 95 fchown -> sys_fchown

fchown(fd, uid, gid): the descriptor form, under the same ownership rule
as the path forms, on the node or the mounted volume the descriptor is
open on. */

### helper krffs_chown_node

The chown rule over a resolved KRFFS node, the POSIX split: changing the
owner takes root; changing the group takes root, or being the owner and
naming one of your own groups. `(id_t)-1` keeps a field, and a no-change
value needs no right at all, so `chown(path, -1, -1)` always succeeds on an
existing file.

### helper vfs_chown_plan

What a chown changes on a mounted file system, and the set-id bits it
takes with it.
`vst` is read from the node and updated in place; the return is the
`VFS_SET_*` mask to apply, 0 when the call changes nothing, or `-EPERM`
when this caller may not make the change. A chown that changes neither
owner is permitted to every caller, as it is on the boot root: a value
that changes nothing needs no right, and a refusal there would stop
`chown(path, -1, -1)`. The set-id clear is `vfs_kill_setid`'s rule, which
is `cred_kill_setid`'s rule for the boot root: a directory keeps its bits,
and a file without group-execute keeps setgid, since there the bit marks
mandatory locking and grants nobody anything.

### helper cred_privileged

Non-zero when the caller has appropriate privilege (effective uid 0). */


## 102 socketcall -> sys_socketcall

Multiplexed socket syscall (socketcall, syscall 102).
EBX is the SC_* sub-call number, ECX a user pointer to its argument array.
Copies the right number of 32-bit args in one fault-checked read, then
dispatches to the matching do_* helper. Returns its result, or -EINVAL for an
unknown sub-call.

### helper do_recvmsg

Receive one datagram into a scatter list (recvmsg, socketcall 17).
The DHCP client is what needs this: it reads its raw socket through
`recvmsg`, not `read` so that it can look for `PACKET_AUXDATA` in the
control data, and finding none it verifies the UDP checksum itself. So the
scatter half is the whole of the work here. The control buffer is answered
with a length of zero, which is the true answer: this stack attaches no
ancillary data to anything.
The sender is reported through `msg_name`: a `sockaddr_in` for an AF_INET
socket, and for a packet socket the `sockaddr_ll` the frame came from, with
the sender's hardware address and who the frame was addressed to.


## 106 stat -> sys_stat

stat(106) follows symlinks; lstat(107) does not. musl falls back to these
from statx for AT_FDCWD paths, so both must exist for stat()/lstat() to work.


## 107 lstat -> sys_lstat

(no comment)


## 114 wait4 -> sys_wait4

sys_wait4 - like waitpid plus an optional rusage out-pointer
(zero-filled). Args: EBX = pid, ECX = status, EDX = options, ESI = rusage.
musl's wait/waitpid are implemented on top of this.

### helper do_wait

Shared core of waitpid(7) and wait4(114).
`status` (may be NULL) receives the Linux-encoded wait status that
sys_exit/terminate already stored in exit_code; `rusage_ptr` (0 for
waitpid, may be 0 for wait4) is zero-filled on reap. With WNOHANG, returns
0 instead of blocking when a child exists but has not exited.


## 116 sysinfo -> sys_sysinfo

System statistics (sysinfo(2)).
Fills the 64-byte i386 struct sysinfo: uptime in seconds, a live-process
count, mem_unit = 1; CPU load and memory/swap figures are reported as zero
(not tracked). EBX = struct sysinfo *.


## 118 fsync -> sys_fsync

Flush a file's data to stable storage (fsync(2); also serves fdatasync).
EBX = fd. Validates the descriptor (any open fd; the type is not checked),
then writes back every dirty page of the disk-backed window via `pc_sync`.
The flush is whole-cache, not per-file, a safe superset of the
contract. When KRFFS is the RAM module the cache is empty, so the flush is
a no-op and the call only validates the fd. Returns 0, -EBADF for a bad
descriptor, or -EIO when a disk writeback fails. dropbear writes its host
key with the open/write/fsync/rename idiom and treats an fsync error as
fatal, so the -EIO is real backpressure.


## 119 sigreturn -> sys_sigreturn

sigreturn (syscall 119): return through musl's legacy __restore (plain
handlers, no SA_SIGINFO). It pops the signal word before the `int $0x80`,
so the frame sits 8 bytes below the user esp. Reads the same kernel-built
frame as the rt path; the popped word is the handler's `sig` argument and
is discarded.

### helper signal_restore

The shared body of sigreturn.
Reads the frame the restorer left on the user stack, validates it, and
restores the saved trapframe (segment selectors and EFLAGS forced to safe
user values, since the frame is on the untrusted user stack) plus the
pre-handler signal mask. Returns the restored EAX so the dispatcher does not
clobber it.
`delta` is how far below the current user esp the frame begins, which
differs by trampoline: musl's __restore_rt enters with esp just past
`pretcode` (delta 4), while the legacy __restore pops the signal word
first and so enters 4 bytes higher (delta 8). Both read the same
kernel-built frame.


## 122 uname -> sys_uname

(no comment)


## 132 getpgid -> sys_getpgid

sys_getpgid - return a process's process-group id (getpgid(2)).
Args: EBX = pid (0 = caller).


## 140 llseek -> sys__llseek

(no comment)

### helper raw_seek_resolve

Resolve a seek on a raw disk window, in unsigned arithmetic throughout.
The obvious form adds the delta to the base as a signed 64-bit value and
then checks the result for negativity, which is undefined the moment the sum
leaves the signed range: a descriptor parked at the top of that range and
seeked forward by one has no defined result to test. So the base is unsigned,
the delta's magnitude is taken without ever negating it (negating the most
negative value is itself undefined), and both directions are bounds-checked
before the arithmetic, not after.
@param cur    the window's current offset.
@param size   the window's byte size, for SEEK_END.
@param delta  the requested displacement, which may be negative.
@param whence 0 SEEK_SET, 1 SEEK_CUR, 2 SEEK_END.
@param out    the resolved offset, untouched on failure.
@return 0, `-EINVAL` for a bad whence or a result before the start, or
        `-EOVERFLOW` for one past what an offset can hold.

### helper file_size_now

The current size of the file an ordinary descriptor is open on.
A `SEEK_END` resolves against this, so the caller reads it inside the
description's lock: a process sharing the description can extend the
file while this one waits.
@return 0, or -errno when the file is gone or its size cannot be read.

### helper seek_add

Add a seek displacement to a file offset without overflowing.
The sum of an offset and a displacement leaves the range of the type
for offsets a caller may legally ask about, and signed overflow has no
value in C to check afterwards, so the bound is tested first and the
arithmetic done unsigned.
@return 0, `-EINVAL` when the result would be negative, or
`-EOVERFLOW` when it would pass the largest offset a file can have.


## 142 newselect -> sys__newselect

(no comment)


## 145 readv -> sys_readv

Scatter read (readv(2)). The twin of `writev` above, and it exists for the
same reason that one does: musl's buffered stdio issues this call rather than
`read` whenever it is filling more than a single byte, so without it every
program that reads through `fgets`, `fread` or `scanf` fails on this system.
That is why several programs in this tree read a byte at a time through
`read` instead, and why busybox's `chpasswd`, which reads its input with
`xmalloc_fgetline`, exited without a message before this existed.
EBX = fd, ECX = struct iovec[], EDX = iovcnt. Returns the total bytes read
across the iovecs, or a negative errno.
The whole vector list is checked before a byte is taken, because a read
cannot be undone: a caller that named a bad destination in its last buffer
gets -EFAULT with its input still queued, not a partial total over
data that is now gone. The check is the one `write` makes of a buffer it is
about to hand a driver, `user_buffer_ok` with write access, so an address
that is merely inside the user window but unmapped or read-only is refused
here and not discovered half way through. A list that asks for nothing still
checks the descriptor, so a closed fd is -EBADF and not a quiet zero.
The buffers are then filled in order, and what "in order" costs depends on
what is behind the descriptor, because a second read is not always the same
thing as reading on.
A regular file is read again for each buffer. Its read returns at once and
its bytes carry no message boundary, so a walk is the single scatter read.
A datagram socket is NOT read again: one datagram is one message, and a
second read would take the next one while the first one's tail was dropped.
The waiting datagram is copied out piece by piece instead, through
`socket_dgram_peek`, and consumed once at the end, so the whole of it fills
as many buffers as it needs and the remainder past the last one is discarded
the way a plain `read` discards it.
A pipe, a stream socket, a device or a `/proc` file is read again only while
it says it has more, since a read of an empty one blocks for input nobody has
written. Asking and then reading is safe for two reasons together: no other
process runs in between, because a syscall is not preemptible here, and the
interrupts that do run can only ADD to what is readable, never take it away.
Nothing else empties a pipe or a receive ring.
A terminal is read exactly once, and that is not a limitation but its
contract: the line discipline hands back one line, and filling a second
buffer with the line after it would return two lines where a reader asked
for one.
Returning less than was asked for is what a reader must already handle, and
musl's stdio, which is what issues this call, loops.

### helper read_fd

Read up to `count` bytes from descriptor `fd` (read(2)).
EBX = fd, ECX = buf, EDX = count. Returns the bytes read, 0 at end, or
-errno. Dispatches by backing kind. A console read (stdin) blocks in the
tty line discipline until a cooked line or VMIN raw bytes arrive; in
graphics mode it is a non-blocking raw scancode read instead.

### helper readv_dgram

Fill the buffers of a `readv` from one waiting datagram.
A datagram is a message, so it is taken once and copied out in pieces: the
buffers are filled in order until the datagram runs out, and what is left
over when the buffers run out first is dropped, which is what a `read` too
small for a datagram already does. Exactly one datagram is consumed either
way, and a datagram of no bytes returns 0 without being end of input.
The message is taken out of the socket whole and under one hold, and only
then spread across the buffers. Both halves of that are deliberate.
Taking it under one hold is not optional: a syscall body runs with interrupts
ENABLED (only preemption is off, see net.h), so the receive interrupt would
otherwise deliver the next datagram over this one between two buffers, and
the caller would get a prefix of one message followed by bytes of another.
Spreading it afterwards, with interrupts on and through `copy_to_user`, is
what keeps a caller's own iovec array from being a weapon. The buffers are
read from user memory as the copy goes, so a caller can point one buffer AT
its own vector list and have the datagram rewrite the destinations behind it.
Those addresses were never checked by the pass that ran before the read.
`copy_to_user` refuses an address it cannot fault in, which makes such a
caller a process scribbling on itself, not a raw kernel write to
whatever it named.
The message buffer is one datagram wide on a 16 KB kernel stack. That is the
whole of what "a kernel buffer to read into" ever needed to be.
`src_ip` and `src_port` report the datagram's sender for a caller that has
somewhere to put it, and are NULL for one that has not; the sender is read
under the same hold as the copy, since consuming is what publishes it.
Returns the bytes copied, or a negative errno.

### helper read_ready

Read into `buf` only if that can be done without waiting. Returns the bytes
read, 0 when there are none, or a negative errno.
This is what fills the second and later buffers of a `readv` on anything
whose bytes arrive, not sit there. Readiness is asked of the backing
object itself: the ops vtable answers `poll`, and a stream socket has a
non-blocking read of its own. Nothing can consume the bytes between the
question and the read, since a system call here runs with interrupts off from
end to end.
A terminal answers 0 whatever it has. Its read returns one line, and a
second line into the next buffer would answer a question the reader did not
ask.


## 146 writev -> sys_writev

(no comment)

### helper write_fd

Write `count` bytes of user buffer `buf` to descriptor `fd`.
The shared body of write(2) and writev(2). Dispatches by backing kind: an
ops-backed object (pipe/tty/pty/device) through its vtable, a socket to the
network, the console through the tty output processor, a KRFFS file, or the
framebuffer. Returns the bytes written or a negative errno.


## 147 getsid -> sys_getsid

sys_getsid - return a process's session id (getsid(2)).
Args: EBX = pid (0 = caller).


## 148 fdatasync -> sys_fsync (the same handler as above)


## 162 nanosleep -> sys_nanosleep

Block-and-schedule sleep (nanosleep, syscall 162, 32-bit timespec).
EBX = const struct timespec* req, ECX = struct timespec* rem (optional).
Rounds up to whole 10 ms ticks, then parks the process BLOCKED on its
wakeup_tick and yields; the timer wakes it at the deadline while OTHER
processes keep running. rem is written as zero on normal completion, or
the time left on -EINTR.


## 168 poll -> sys_poll

(no comment)


## 173 rt_sigreturn -> sys_rt_sigreturn

rt_sigreturn (syscall 173): return through musl's __restore_rt (SA_SIGINFO
handlers), a bare `int $0x80`, so the frame sits 4 bytes below the user esp.

### helper signal_restore

The shared body of sigreturn.
Reads the frame the restorer left on the user stack, validates it, and
restores the saved trapframe (segment selectors and EFLAGS forced to safe
user values, since the frame is on the untrusted user stack) plus the
pre-handler signal mask. Returns the restored EAX so the dispatcher does not
clobber it.
`delta` is how far below the current user esp the frame begins, which
differs by trampoline: musl's __restore_rt enters with esp just past
`pretcode` (delta 4), while the legacy __restore pops the signal word
first and so enters 4 bytes higher (delta 8). Both read the same
kernel-built frame.


## 174 rt_sigaction -> sys_rt_sigaction

Install or query a signal disposition (rt_sigaction, syscall 174).
EBX=signo, ECX=const struct k_sigaction* act, EDX=struct k_sigaction* old,
ESI=sigsetsize. Stores the action per-signal; SIGKILL/SIGSTOP cannot be
changed.


## 175 rt_sigprocmask -> sys_rt_sigprocmask

Examine or change the blocked-signal mask (rt_sigprocmask, syscall 175).
EBX=how (0 BLOCK/1 UNBLOCK/2 SETMASK), ECX=const set*, EDX=old set*,
ESI=sigsetsize. SIGKILL/SIGSTOP can never be blocked.


## 179 rt_sigsuspend -> sys_rt_sigsuspend

Atomically install a signal mask and wait (rt_sigsuspend(2)).
EBX = const sigset_t *mask, ECX = sigsetsize (8). Replaces the blocked mask
with *mask, waits for a deliverable signal, restores the old mask, then
returns -EINTR. The handler runs under the RESTORED mask: a simplification,
since full save-the-mask-through-the-handler semantics are not modelled.


## 182 chown -> sys_chown (the same handler as above)


## 183 getcwd -> sys_getcwd

getcwd(2): EBX = buf, ECX = size. Returns the path length including the
NUL, or -errno. */


## 190 vfork -> sys_fork (the same handler as above)


## 191 ugetrlimit -> sys_getrlimit (the same handler as above)


## 192 mmap2 -> sys_mmap2

sys_mmap2 - map anonymous memory. Args: EBX=addr (hint, ignored),
ECX=length, EDX=prot, ESI=flags, EDI=fd, EBP=pgoffset. Returns the mapped
base address, or -errno. MAP_ANONYMOUS is required and MAP_FIXED refused;
MAP_PRIVATE and MAP_SHARED are not checked, and every mapping is private
(what musl's allocator uses); the region is placed by bumping mmap_top
down and is demand-paged + zero-filled on touch.

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


## 194 ftruncate64 -> sys_ftruncate64

(no comment)

### helper truncate_fd

Truncate an open file to `length` (ftruncate(2)).
The descriptor must be a writable file on a mounted file system. A
successful shrink or extend clears the set-id bits for an unprivileged
caller, like a write. `SYS_FTRUNCATE` takes the length in ECX, and
`SYS_FTRUNCATE64`, the form musl calls, in ECX and EDX as the low and
high halves; the file system answers a length past what it can hold.


## 195 stat64 -> sys_stat64

stat64(195) follows symlinks; lstat64(196) does not. On i386 musl maps its
stat()/lstat() onto these (via the statx -> *stat64 fallback), so lstat()
only works once 196 exists. The follow flag decides whether a symbolic
link at the end, on /proc or on a mounted type, is the result or its
target. */


## 196 lstat64 -> sys_lstat64

(no comment)


## 197 fstat64 -> sys_fstat64

fstat64 (syscall 197): EBX = fd, ECX = stat64 buffer. The descriptor's
node is stat'd; a root directory fd (node == NULL) reports the root
directory.

### helper fstat_fill

Fill a stat64 from an open descriptor by type.
Shared by sys_fstat64 and the followed /proc/<pid>/fd/<n> path (whose
target is just this fd). Where the object has an identity of its own (a
socket, a pipe, a device, a /proc snapshot, a KRFFS node), the inode
reported here is that identity and is the same number the descriptor's
`/proc/<pid>/fd` link carries, so a program reading one can match the other.


## 198 lchown32 -> sys_chown (the same handler as above)


## 199 getuid32 -> sys_getuid

Identity (the i386 *32 variants, 199-202): the four getters read the PCB
credentials, all zero (root) until a login server drops them. The setters
and the group list live with the rest of the id family near do_chown.


## 200 getgid32 -> sys_getgid

(no comment)


## 201 geteuid32 -> sys_geteuid

(no comment)


## 202 getegid32 -> sys_getegid

(no comment)


## 203 setreuid32 -> sys_setreuid

(no comment)

### helper cred_privileged

Non-zero when the caller has appropriate privilege (effective uid 0). */


## 204 setregid32 -> sys_setregid

(no comment)

### helper cred_privileged

Non-zero when the caller has appropriate privilege (effective uid 0). */


## 205 getgroups32 -> sys_getgroups

(no comment)


## 206 setgroups32 -> sys_setgroups

setgroups/getgroups (the *32 forms): the supplementary list, privileged to
set (effective uid 0), NGROUPS_MAX entries. getgroups with size 0 reports the
count, the POSIX sizing idiom. */

### helper cred_privileged

Non-zero when the caller has appropriate privilege (effective uid 0). */


## 207 fchown32 -> sys_fchown (the same handler as above)


## 208 setresuid32 -> sys_setresuid

(no comment)

### helper cred_privileged

Non-zero when the caller has appropriate privilege (effective uid 0). */


## 209 getresuid32 -> sys_getresuid

getresuid/getresgid (the *32 forms): report all three ids through the three
user pointers, the introspection half of the saved-id model. */


## 210 setresgid32 -> sys_setresgid

(no comment)

### helper cred_privileged

Non-zero when the caller has appropriate privilege (effective uid 0). */


## 211 getresgid32 -> sys_getresgid

(no comment)


## 212 chown32 -> sys_chown (the same handler as above)


## 213 setuid32 -> sys_setuid

(no comment)

### helper cred_privileged

Non-zero when the caller has appropriate privilege (effective uid 0). */


## 214 setgid32 -> sys_setgid

(no comment)

### helper cred_privileged

Non-zero when the caller has appropriate privilege (effective uid 0). */


## 220 getdents64 -> sys_getdents64

Read directory entries (getdents64(2)).
EBX = fd, ECX = dirp buffer, EDX = count. Packs linux_dirent64 records
into the buffer, one per child, and advances the fd's offset cursor.
Returns the bytes written, 0 at end of directory, or -errno.

### helper fd_node_check

Re-validate a descriptor's cached KRFFS node before a file operation.
A held node is never freed now, so this stops being an identity test and
becomes what it always was in fact, a resolution. Two shapes are live: an
orphan, which no path names and which nothing will free while this descriptor
holds it; and a named node, which must still resolve back to itself. Anything
else is a descriptor whose file went away by a route that did not consult the
holders, and stays -EBADF.
The old round-trip could not tell a freed node from a different file the
allocator later placed at the same address. That hole is closed for every
regular-file descriptor by the lifetime, not by this check: no address
a descriptor has is handed out while it has it.
A NULL node is the root directory and needs no check. Directories are
deliberately out of scope: `rmdir` still frees the node under an open
directory descriptor, so a relative path through one still answers -EBADF.


## 221 fcntl64 -> sys_fcntl64

Descriptor control (fcntl64, syscall 221).
EBX=fd, ECX=cmd, EDX=arg. Supports the descriptor-flag and status-flag
round-trips libc needs: F_DUPFD(_CLOEXEC), F_GETFD/F_SETFD (the
close-on-exec bit), and F_GETFL/F_SETFL (access + O_NONBLOCK/O_APPEND,
on the shared open-file description, which the write and socket paths
read).


## 238 tkill -> sys_tkill

(no comment)


## 243 set_thread_area -> sys_set_thread_area

Set up thread-local storage (set_thread_area(2), Linux i386).
EBX points at a struct user_desc. Validates it, programs the shared TLS
GDT descriptor (entry GDT_TLS_ENTRY) and this process's PCB base, then
writes the chosen entry number back so the C library can load
(entry_number << 3) | 3 into %gs. The base is reapplied to the GDT on
every context switch (switch.s). Returns 0 or a negative errno on a bad
pointer/request.


## 252 exit_group -> sys_exit (the same handler as above)


## 258 set_tid_address -> sys_set_tid_address

Record the clear-child-tid pointer and return the tid (set_tid_address(2)).
Rach2 has no threads, so this only returns the caller's PID as its thread id;
the pointer (EBX) is accepted and ignored.


## 265 clock_gettime -> sys_clock_gettime

Read a POSIX clock (clock_gettime, syscall 265, 32-bit timespec).
EBX = clockid, ECX = struct timespec*. CLOCK_REALTIME(0) reports real time
from the clock read at boot (sys/rtc.c); CLOCK_MONOTONIC(1) reports time
since boot, which is what the scheduler, the sleep path and the audio timing
loops mean by a clock. Both run at the 10 ms tick resolution, and neither is
disciplined by anything: the machine reads the CMOS once and then keeps its
own time. musl falls back here from clock_gettime64 (403), which this kernel
does not provide, so times are 32-bit here and this ABI ends in 2038.


## 268 statfs64 -> sys_statfs64

statfs64(2): the file system a path is on.
EBX is the path, ECX the caller's sizeof(struct statfs64), EDX the buffer.
The size is checked rather than ignored: a caller built against a different
layout would otherwise be handed bytes it would read as different fields.

### helper statfs_magic_for_kind

Which of the three mounts a classified path belongs to. */

### helper statfs_fill_mount

A statfs64 from a mount's own statfs: the numbers vfat reports. */

### helper statfs_fill

Fill a statfs64 for one of the three file systems this machine mounts.
Only the root has blocks: it is KRFFS, and the region's size and its free
nodes' usable bytes are both real, reported in 1024-byte units because that
is what `df` prints in by default. The two synthetic mounts report their own
magic and no blocks at all, which is what they have and what Linux reports
for them; answering with the root's numbers would tell `df` that `/proc` is
a 16 MB file system 77 percent full. `f_files` is zero throughout, which says
"not counted" rather than claiming an inode budget this format does not keep.


## 269 fstatfs64 -> sys_fstatfs64

fstatfs64(2): the same numbers, named by an open descriptor.
EBX is the descriptor, ECX the caller's size, EDX the buffer.

### helper statfs_fill

Fill a statfs64 for one of the three file systems this machine mounts.
Only the root has blocks: it is KRFFS, and the region's size and its free
nodes' usable bytes are both real, reported in 1024-byte units because that
is what `df` prints in by default. The two synthetic mounts report their own
magic and no blocks at all, which is what they have and what Linux reports
for them; answering with the root's numbers would tell `df` that `/proc` is
a 16 MB file system 77 percent full. `f_files` is zero throughout, which says
"not counted" rather than claiming an inode budget this format does not keep.

### helper statfs_fill_mount

A statfs64 from a mount's own statfs: the numbers vfat reports. */


## 270 tgkill -> sys_tgkill

(no comment)


## 295 openat -> sys_openat

openat (syscall 295): EBX=dirfd, ECX=path, EDX=flags, ESI=mode. musl
issues openat (not the legacy open) for every <stdio.h> file open. An
absolute path is taken as-is, a relative path resolves against the cwd
(AT_FDCWD) or against the directory the dirfd refers to (see
copy_path_at).


## 298 fchownat -> sys_fchownat

(no comment)


## 300 fstatat64 -> sys_fstatat64

fstatat64 (syscall 300): EBX = dirfd, ECX = path, EDX = stat64 buffer, ESI
= flags. The path resolves the same way as openat (absolute, or relative
to AT_FDCWD/the dirfd; see copy_path_at). AT_SYMLINK_NOFOLLOW is honored
for the /proc magic symlinks; KRFFS has none.

### helper stat64_root_times

The root directory's own three times, into a stat64.
The root is a real node and creating an entry in it stamps its times, so it
reports them like any other directory; a machine with no file system mounted
has nothing to report but the boot time.


## 303 linkat -> sys_linkat

(no comment)


## 304 symlinkat -> sys_symlinkat

(no comment)


## 305 readlinkat -> sys_readlinkat

(no comment)

### helper do_readlink

readlink core (readlink(2)/readlinkat(2)).
The links are /proc/self, the /proc/<pid>/fd/<n> magic links (which
ttyname reads), and the symbolic links of a mounted type that has them,
read through the type. The result is the byte count and is NOT
NUL-terminated (the readlink contract; musl appends its own terminator and
treats a full buffer as truncation). `name` is the path, leading '/'
stripped.


## 306 fchmodat -> sys_fchmodat

(no comment)

### helper do_chmod

chmod core for the chmod/chown family.
A KRFFS path stores through `krffs_chmod_node` under the owner-or-root rule.
A pty slave (`/dev/pts/N`) stores on the pty, the state dropbear moves
before handing the terminal to the user. Every other synthetic /dev or
/proc object has fixed root-owned metadata: a no-change request validates
and succeeds, a change is refused. `name` is absolute with the leading '/'
stripped, matching do_access.


## 307 faccessat -> sys_faccessat

(no comment)

### helper do_access

access core: does the path exist and grant the requested mode?
R/W/X are checked against the object's mode bits by the caller's REAL ids
(the credential model access(2) uniquely uses), which is what makes
/proc/<pid>/stat reject W_OK while granting F_OK/R_OK. `name` is absolute
with the leading '/' stripped.


## 308 pselect6 -> sys_pselect6

(no comment)


## 309 ppoll -> sys_ppoll

(no comment)


## 320 utimensat -> sys_utimensat

Update a file's access and modification timestamps (utimensat(2)).
EBX = dirfd, ECX = path, EDX = times, ESI = flags. `times` is a 2-element
timespec array [atime, mtime]; a NULL `times` sets both to the current
time. A tv_nsec of UTIME_NOW means "now", UTIME_OMIT means "leave
unchanged". Returns 0 or -errno.

### helper cred_privileged

Non-zero when the caller has appropriate privilege (effective uid 0). */

### helper cred_access_ref

The permission check over a node of any type, from the attributes its
file system reports: the same nine-bit rule as cred_access_node, plus
the execute rule for a regular file.

### helper cred_access_node

The permission check over a KRFFS node. The root directory reads as 0755,
the fixed identity stat reports (its stored mode has the dirty-mount
flag). Executing a regular file demands an execute bit somewhere even for
root: root runs programs, not data.


## 330 dup3 -> sys_dup3

dup3(330): like dup2 but oldfd == newfd is -EINVAL and a flag can set
O_CLOEXEC on the new descriptor. */


## 331 pipe2 -> sys_pipe2

(no comment)


## 340 prlimit64 -> sys_prlimit64

prlimit64(340): EBX=pid (0/self), ECX=resource, EDX=new (rlimit64* or NULL),
ESI=old (rlimit64* or NULL). 64-bit struct rlimit64 { u64 cur, max }. The
old limits are reported before the new are set, as POSIX's `setrlimit`
and `getrlimit` see them. This is the form musl issues for both. */

### helper rlimit_set

Set a kept limit of `p`: the soft value at most the hard, the hard raised
by root only and lowered by anyone, and RLIMIT_NOFILE never above the
table's limit. A limit that is not kept is accepted and not changed.
0, or -EINVAL and -EPERM as POSIX names them.


## 355 getrandom -> sys_getrandom

Fill a buffer with random bytes (getrandom, syscall 355).
EBX = buf, ECX = buflen, EDX = flags. Backed by the kernel CSPRNG
(lib/krandom, ChaCha20). Returns the byte count, like Linux; musl's
getrandom/arc4random sit on top of this. We never block (GRND_NONBLOCK is
a no-op) and have a single source (GRND_RANDOM is equivalent); unknown
flag bits are rejected.


## socketcall operations (SYS_SOCKETCALL 102)


### SC_SOCKET = 1 -> do_socket

socket(2): only AF_INET (domain 2), SOCK_STREAM, and SOCK_DGRAM. Returns a
new socket fd or -errno. */


### SC_BIND = 2 -> do_bind

bind(2): record the local port from the sockaddr; a datagram socket also
registers it with UDP (-EADDRINUSE if taken). */


### SC_CONNECT = 3 -> do_connect

connect(2): start the TCP handshake to the sockaddr. A blocking socket waits
it out (-EINTR on a signal); a non-blocking one returns 0 or -EINPROGRESS. */


### SC_LISTEN = 4 -> do_listen

listen(2): mark a stream socket passive and open its LISTEN TCB. */


### SC_ACCEPT = 5 -> do_accept

accept(2): dequeue a completed connection (blocking, or -EAGAIN on a
non-blocking listener), install a new fd for it, and report the peer address.


### SC_GETSOCKNAME = 6 -> do_getsockname

getsockname: the socket's local address, the single NIC IP (net_ip_addr) plus
its bound or ephemeral port. */


### SC_GETPEERNAME = 7 -> do_getpeername

getpeername: the connected peer's address (a connected stream socket
only). */


### SC_SEND = 9 -> do_send

send(2): write to a connected stream socket (the peer is the
connection's). */


### SC_RECV = 10 -> do_recv

recv(2): read from a connected stream socket. Honors MSG_DONTWAIT and
O_NONBLOCK. */


### SC_SENDTO = 11 -> do_sendto

sendto(2): a stream socket ignores the destination and behaves like send; a
datagram socket sends to the sockaddr, binding an ephemeral source port
first. */


### SC_RECVFROM = 12 -> do_recvfrom

recvfrom(2): a stream socket behaves like recv (no source writeback); a
datagram socket reports the source address, truncated to the caller's
capacity. Honors MSG_DONTWAIT and O_NONBLOCK. */


### SC_SHUTDOWN = 13 -> do_shutdown

(no comment)


### SC_SETSOCKOPT = 14 -> do_setsockopt

setsockopt: accept the boolean options a forking server sets (SO_REUSEADDR,
TCP_NODELAY, SO_KEEPALIVE) so its setup succeeds. The stack already reuses
listen ports and sends promptly, so these are honored-by-behavior or
documented no-ops; an unknown option is accepted instead of failing a
server's startup. */


### SC_GETSOCKOPT = 15 -> do_getsockopt

getsockopt: report the integer options programs read at setup: SO_ERROR (no
async error is tracked, so 0), SO_TYPE (the real socket type), and the
boolean options as 0 (off). Always reports a 4-byte int result. */


### SC_RECVMSG = 17 -> do_recvmsg

Receive one datagram into a scatter list (recvmsg, socketcall 17).
The DHCP client is what needs this: it reads its raw socket through
`recvmsg`, not `read` so that it can look for `PACKET_AUXDATA` in the
control data, and finding none it verifies the UDP checksum itself. So the
scatter half is the whole of the work here. The control buffer is answered
with a length of zero, which is the true answer: this stack attaches no
ancillary data to anything.
The sender is reported through `msg_name`: a `sockaddr_in` for an AF_INET
socket, and for a packet socket the `sockaddr_ll` the frame came from, with
the sender's hardware address and who the frame was addressed to.


### SC_PING = 0x1000 -> do_ping

(no comment)
