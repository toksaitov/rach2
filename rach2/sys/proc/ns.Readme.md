# Namespaces

A namespace is a view of one part of the system that the processes in it share. `ns.c` has Linux's six kinds, `NS_KINDS` in `ns.h`: a user namespace maps user and group ids and scopes privilege; a PID namespace numbers its processes from 1; a mount namespace has its own mount table; a network namespace has its own sockets and no interface; a UTS namespace has its own host and domain names; and an IPC namespace has its own System V objects and POSIX message queues. They are what `unshare(1)`, `nsenter(1)` and a browser's sandbox ask for. The calls' contracts are in [unshare(2)](../../../man/man2/unshare.2), and the files of `/proc/PID/ns` and a user namespace's maps in [proc(5)](../../../man/man5/proc.5); this document describes the records and what each kind changes in the rest of the kernel.

## Interfaces

| Call | i686 | x86-64 | Manual page |
| --- | --- | --- | --- |
| `unshare` | 310 | 272 | [unshare(2)](../../../man/man2/unshare.2) |
| `setns` | 346 | 308 | [unshare(2)](../../../man/man2/unshare.2) |
| `clone` | 120 | 56 | [fork(2)](../../../man/man2/fork.2) |
| `clone3` | 435 | 435 | [fork(2)](../../../man/man2/fork.2) |

* `unshare`, `clone` and `clone3` take the flags `CLONE_NEWUSER`, `CLONE_NEWPID`, `CLONE_NEWNS`, `CLONE_NEWNET`, `CLONE_NEWUTS` and `CLONE_NEWIPC` (`ns.h`).
* `/proc/PID/ns` has a link for each kind, whose target is `KIND:[INUM]` (`ns_link_name`). An open of one gives a descriptor on the namespace (`nsfs_open`), which `setns` takes and on which `ioctl` answers `NS_GET_USERNS`, `NS_GET_PARENT`, `NS_GET_NSTYPE` and `NS_GET_OWNER_UID` (`nsfs_ioctl`).
* `/proc/PID/uid_map`, `gid_map` and `setgroups` read and write a user namespace's maps (`ns_map_text`, `ns_map_write`).
* Inside the kernel, `uid_seen`, `gid_seen`, `uid_named` and `gid_named` translate ids, and `pid_seen`, `pid_seen_by` and `pid_named` process ids, between the kernel's and the current process's view; `ns_privileged` and `ns_owner_privileged` answer privilege; `proc_userns` to `proc_ipcns` give a process's namespaces; and `ns_fork`, `ns_exec`, `ns_release`, `ns_reaper` and `ns_reaper_gone` are called by fork, exec, the release of a record and a process's end ([processes](Readme.md)).

## Design

### Records

Every namespace begins with `struct ns_common`: its references, its inode number, its kind, and the user namespace that owns it. The first namespace of each kind is a static record (`init_userns`, `init_pidns` and the others in `ns.c`), which is never freed; those of the user, PID, UTS and IPC kinds have the inode numbers Linux gives its first ones, and every later namespace is numbered upward from `0xF0000100`.

A process record has its own user namespace and PID namespace, its numbers in the PID namespaces it is in (`nspid`), and a pointer to a `struct nsset` with its mount, network, UTS and IPC namespaces and the PID namespace its next children are made in, as Linux's nsproxy has them. A NULL pointer in a record or a set stands for the first namespace of the kind. The processes that inherit a set share it, counted, and `unshare` and `setns` give the caller a copy with its kinds changed. Each namespace counts the records and sets that refer to it; a namespace is also a reference on the user namespace that owns it, and a user or PID namespace on its parent. The last reference's drop frees the namespace and does what the end of its kind does (below).

### User namespaces and ids

A process's ids stay the kernel's, those of the first user namespace, everywhere: the permission checks, the owners of files and the credentials compare kernel ids, as Linux's `kuid_t` does. A user namespace changes only what the calls that report or take an id see. `uid_seen` gives the current process's view of a kernel id, or `NS_OVERFLOW_ID`, 65534, for one its maps do not cover; `uid_named` gives the kernel id of an id the caller names, or -1, which the calls answer with `EINVAL`. The id calls, `stat`, `chown`, the System V and socket credentials, the files of `/proc` and the siginfo of a child's end translate at that boundary.

A map has at most `NS_EXTENTS`, 5, extents, each a first id in the namespace, the first of the matching ids in its parent, and a count, and keeps the parent's ids as kernel ids. A map is written once (`ns_map_write`), by a process in the namespace or its parent that is privileged in the namespace, and maps only ids the parent maps; extents that overlap are refused. A writer not privileged in the parent may map one id, its own effective one, and for the groups only after it wrote `deny` to `/proc/PID/setgroups`, which then refuses `setgroups` in the namespace and those below it.

Privilege follows Linux's `cap_capable` for a process that keeps every capability or none (`ns_privileged`). A process privileged in its own user namespace is privileged in every user namespace below it, and over every namespace those own; a process in a namespace's parent with the effective user id of the process that made the namespace is privileged in it too. In its own namespace, when that is not the first, a process is privileged while it has capabilities there (`ns_caps` in the record), and after that as effective user id 0 there. Making or joining the namespace gives it every capability; an `execve`, a change of the effective user id, or `capset` takes them away. `proc_privileged` (`process.h`) stays the test of privilege over the whole system: effective user id 0 in the first user namespace. User namespaces nest at most 7 below the first (`NS_LEVELS` in `process.h` counts the first as level 0), and so do PID namespaces; a deeper one is `EUSERS`.

### PID namespaces

A process has a number in each PID namespace from its own up to the first (`nspid`, by level), each given at the fork (`ns_number`), and its kernel id is its number in the first. A namespace other than the first gives out numbers from 1 to `NS_PID_MAX`, 32768, then from 2 again, past the numbers its processes have. `pid_seen` and `pid_named` translate at the boundary as the id functions do: a process outside the caller's namespace and those below it is seen as 0 and cannot be named.

The first process made in a namespace is its init, the namespace's `reaper`. Children whose parent ends are given to it (`ns_reaper`, which `reparent_children` asks), and its end sends every other process of the namespace SIGKILL (`ns_reaper_gone`) and leaves the namespace without one, in which a fork fails with `ENOMEM`. `CLONE_NEWPID` given to `unshare`, and `setns` to a PID namespace, do not move the caller: they change the namespace its next children are made in.

### Mount namespaces

A mount namespace is a bit of a 32-bit mask (`NS_MNT_MAX`), so 32 exist at once, the first's bit among them, and one more is `ENOSPC`. Each mount has the bits of the namespaces that do not see it (`ns_hidden` in `rach2/sys/fs/vfs.h`). A new mount is hidden from every namespace but its maker's; a new namespace sees what its parent sees (`vfs_mntns_copy`); and an unmount hides the mount from the caller's namespace and unmounts it only when no other namespace sees it. The resolver, the walks of the mount table and `/proc/mounts` pass over a mount the caller's namespace does not see (`vfs_mount_seen`), and a namespace's end unmounts what it alone saw (`vfs_mntns_gone`). [The VFS](../fs/vfs.Readme.md) describes the mount table.

### Network, UTS and IPC namespaces

A network namespace other than the first has no interface, the loopback included. A socket keeps the namespace it was made in (`ns` in `rach2/sys/network/socket/socket.h`); outside the first, a bind and a listen succeed and nothing is delivered to them, and a connection or a send fails with `ENETUNREACH` (`socket_outside` in `rach2/sys/syscall_net.c`). The interfaces, the routes and the ARP table of `/proc/net`, `/sys/class/net`, the routing socket and a packet socket's interface numbers show such a namespace nothing (`net_ns_seen` and `netdev_seen_count` in `rach2/sys/network/netif.c`), and each namespace's socket tables list its own sockets. Unix sockets keep their namespace too, and an abstract name is looked up among the caller's namespace's sockets (`rach2/sys/ipc/unix.c`). A namespace's end moves its sockets to a namespace that has ended (`net_ns_gone` in `rach2/sys/network/socket/socket.c`).

A UTS namespace has its own host and domain names, a copy of its parent's when it is made; `uname`, `sethostname` and `setdomainname` read and write the caller's (`rach2/sys/syscall.c`). The first's are `rach2` and `(none)`.

An IPC namespace tags each System V object and POSIX message queue with the namespace it was made in, and a lookup finds only the caller's namespace's (`rach2/sys/ipc/sysv.c`, `rach2/sys/ipc/mqueue.c`). Its end removes its semaphore sets and message queues, takes away the names of its POSIX message queues, and marks its shared memory segments for removal; a segment goes once no process has it attached, and a queue with its last descriptor (`ipc_ns_gone`).

## Limits

* There are six kinds; the cgroup and time namespaces are refused with `EINVAL`.
* At most 32 mount namespaces exist at once, and user and PID namespaces nest at most 7 below the first.
* A map of a user namespace has at most 5 extents, Linux's limit before 4.15.
* A network namespace other than the first has no interface, the loopback included, and no way to be given one.
* A PID namespace other than the first numbers its processes up to 32768.

## Tests

`tests/run default` runs `tests/musl/nsmusl.c`, and `ARCH=x86_64 tests/run default` runs it on x86-64 ([the tests' Readme](../../../tests/Readme.md)). It makes a namespace of each kind and checks what it changes: a host name, the numbers of a PID namespace's first process and its child, a mount not seen outside, a connection to the loopback address that fails with `ENETUNREACH`, and a System V object the new IPC namespace does not have. As user 1000 it makes a user, a PID and a network namespace in one `clone`, as a browser's sandbox does, and in a user namespace of its own it writes the maps, reads them back, and runs a set-user-id program of root's as itself.

The ten faults of `tests/system/probe_faults.txt` whose names begin with `ns-` must make `nsmusl` fail; `sh tests/system/probe_faults.sh NAME` runs one. No host test covers `ns.c`.
