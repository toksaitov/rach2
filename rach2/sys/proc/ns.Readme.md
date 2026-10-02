# Namespaces

`ns.c` implements Linux's user, PID, mount, network, UTS and IPC namespaces, as `unshare(1)`, `nsenter(1)` and a browser's sandbox ask for them: `unshare(2)`, system call 310, `setns(2)`, 346, and the `CLONE_NEW` flags of `clone(2)`. `man/man2/unshare.2` gives the user contract, and `man/man5/proc.5` the files of `/proc/PID/ns` and the user namespace's maps.

## Records

A namespace begins with `struct ns_common`: its references, its inode number, its kind and the user namespace that owns it. The first namespace of each kind is a static record (`init_userns` and the others), with Linux's inode numbers for the user, PID, UTS and IPC kinds. A process record has its user and PID namespaces and a pointer to a shared `struct nsset` with the other four and the PID namespace its next children are made in, as Linux's nsproxy has them; `unshare` and `setns` give the caller a copy of its set. A NULL pointer is the first namespace of that kind.

## IDs

A process's IDs stay the kernel's, those of the first user namespace, everywhere: the permission checks, the owners of files and the credentials compare kernel IDs, as Linux's kuid_t does. A user namespace changes only what the calls that take or report an ID see. `uid_seen` gives the current process's view of a kernel ID, and NS_OVERFLOW_ID, 65534, for one its maps do not cover; `uid_named` gives the kernel ID of an ID the caller names, or -1, which the calls answer with `EINVAL`. The getters, the setters, `stat`, `chown`, the System V and socket credentials and the files of `/proc` translate at that boundary. A map has at most five extents, and its lower IDs are stored as kernel IDs.

Privilege follows Linux's `cap_capable` for a process that holds all capabilities or none: privileged in its own user namespace, and so in every namespace that namespace owns, or the owner of a child of its namespace (`ns_privileged`). A process is privileged in a namespace it made until an `execve` or a change of its effective user ID (`ns_caps`), and after that as effective user ID 0 there. `proc_privileged` stays the test of privilege over the whole system: effective user ID 0 in the first user namespace.

## PIDs

A process has a number in each PID namespace from its own up to the first (`nspid`), given at the fork; the kernel's PID is the number in the first. `pid_seen` and `pid_named` translate at the boundary as the ID functions do, and a process not in the caller's namespace or below it is seen as 0 and cannot be named. The first process of a namespace is its reaper: children whose parent ends are given to it (`ns_reaper`), and its end kills the rest of the namespace (`ns_reaper_gone`) and leaves the namespace without one, in which no process may be made.

## Mounts, network, UTS and IPC

A mount namespace is a bit of a 32-bit mask, so 32 may exist at once. A mount has the bits of the namespaces that do not see it (`ns_hidden` in `fs/vfs.h`); a mount is made hidden from every namespace but the caller's, a new namespace takes the bits of its parent's mounts (`vfs_mntns_copy`), and an unmount hides the mount from the caller's namespace and unmounts it only when no namespace sees it. The lookups and `/proc/mounts` skip a mount the caller's namespace does not see.

A network namespace other than the first has no interface. A socket remembers the namespace it was made in (`socket.h`); outside the first, a bind and a listen succeed and deliver nothing, and a connection or a send fails with `ENETUNREACH`. The tables of `/proc/net`, `/sys/class/net`, the routing socket and the packet sockets show such a namespace nothing (`net_ns_seen`), and the abstract names of Unix sockets are looked up in the caller's namespace.

A UTS namespace has its own host and domain names. An IPC namespace tags its System V objects and message queues, a lookup sees only the caller's namespace's, and its end removes them (`ipc_ns_gone`).

## Tests

`tests/musl/nsmusl.c`, in the default suite, makes a namespace of each kind and checks what it changes, makes a user, a PID and a network namespace in one clone as an ordinary user, and writes the maps of a user namespace. The faults `ns-*` of `tests/system/probe_faults.txt` must make it fail.
