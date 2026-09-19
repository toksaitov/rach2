# limits(7)

NAME: limits - the resource limits a process has and where they come from. Sources: rach2/Readme.md "Hardware and limits" (the prlimit64 paragraph and the process, descriptor, socket paragraphs); utils/Readme.md "Init and account programs" (/etc/rach2/limits); the staged `/etc/rach2/limits` text in krffs/manifests/os-real.manifest (quote its values).

* Three limits per process, each a soft value enforced and a hard value bounding it, kept by `prlimit64(2)` (`getrlimit`, `setrlimit`, `ugetrlimit` are the same table). Fork inherits both; exec preserves them.
* `RLIMIT_NPROC`: processes with the same real uid across the machine; fork returns EAGAIN at the soft limit; real uid 0 exempt; a `setuid` moves the process to the new user's count.
* `RLIMIT_NOFILE`: at most 1024; one more than the highest descriptor the process may be given; open returns EMFILE when none below the soft limit is free; `dup2` returns EBADF for a destination at or above it. A descriptor table starts at 16 entries and grows to 1024.
* `RLIMIT_AS`: the bytes covered by the process's virtual memory regions; growth past the soft limit makes `mmap` return ENOMEM and leaves the break unchanged (`brk` fails).
* Other limits the kernel keeps outside this table: process records come from the kernel heap (a fork the heap cannot serve is ENOMEM); sockets and TCP control blocks likewise; the loopback ring 64 frames; twelve USB table entries; eight disk records.
* `/etc/rach2/limits`: read by init at boot, set on init itself before its children, so every session inherits them; a line is `nproc`, `nofile` or `as`, then the soft and the hard value, decimal or `unlimited`; `as` is bytes; a missing file leaves the kernel defaults; `ulimit` in sh(1) reads and lowers them.
* EXAMPLE: `cat /etc/rach2/limits`; the default suite's `limitprobe` proves the three refusals by name.
* SEE ALSO: prlimit64(2) (phase 3), memory(7), sh(1).
