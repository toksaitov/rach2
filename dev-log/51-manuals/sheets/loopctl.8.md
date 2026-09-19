# loopctl(8)

NAME: loopctl - attach a file to a loop device, detach one, or list them. Source: utils/loopctl.c; the devices: rach2/sys/block/Readme.md "The loop device".

* SYNOPSIS: `loopctl` (list), `loopctl attach [-r] [N] FILE`, `loopctl detach N`. Anything else: `usage: loopctl [attach [-r] [N] FILE | detach N]` on stderr, exit 2.
* `attach FILE` takes the first free device; `attach N FILE` takes device N; `-r` attaches a device that will not write (the kernel marks it read-only, and every mount of it is read-only whatever the caller asked). `detach N` gives the device back.
* With no arguments it prints `/proc/loop`: one line per attached device: the device, its size in bytes, whether it takes writes, how many writes and how many syncs it has made, and the file it was attached by.
* It speaks to the kernel through `/dev/loop-control`, one command per write: `attach N /path`, `attach - /path`, `attach ro N /path`, `detach N`. The node is 0600 root's, so only root attaches. `/dev/loop0` to `/dev/loop3` are the devices; `stat` reports them as character devices; only `mount(2)` reaches their bytes.
* Errors are the kernel's, printed as `loopctl: /dev/loop-control: ERROR` or `loopctl: ERROR` (exit 1): EACCES for a caller that is not root; EBUSY for a file already attached (by mount and node, so a hard link is the same file), for a device with a mount placed on it or a buffer held, and for a detach during an attachment's sleeping steps; ELOOP for a file on a loop-mounted volume (a chain may be one device long); EINVAL for a length that is not whole sectors, a device number that is not one of the four, or a file that is not a regular file; `/proc/loop` unreadable: `loopctl: /proc/loop: ERROR`.
* While attached, the file cannot be written, truncated, unlinked or renamed over (EBUSY); its mount cannot be unmounted until the device is detached; a stick that is pulled takes its loop device with it.
* EXAMPLE: `loopctl attach /demo/ext2.img` prints nothing; `loopctl` lists `loop0`; `mount -t ext2 /dev/loop0 /mnt`; `umount /mnt; loopctl detach 0`. `tests/run loopdev` runs the attach, mount, write, unmount and detach.
* SEE ALSO: mount(8), disk(7).
