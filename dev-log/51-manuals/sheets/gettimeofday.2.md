# gettimeofday(2)

NAME: gettimeofday - the system call family: gettimeofday, clock_gettime, times, utimensat.

The page names every member in NAME with .Nm, gives each its prototype in SYNOPSIS as musl declares it (the header and the C prototype; the number in the table is the ABI, EAX), describes the behaviour of this kernel with its differences from Linux where a caller would notice, lists every error the handler returns in ERRORS with when, and ties an example to a probe of the default suite where one exists (tests/*.c). Aliases: every member other than the page name goes into man/aliases as `MEMBER 2 gettimeofday`; the writer adds those lines.

Notes from the milestone: The clocks: the RTC at boot plus ticks; the PIT microsecond clock; which clock ids clock_gettime answers; utimensat on which file systems.

Sources: the handler comments below, extracted from rach2/sys/syscall.c (read the handler and the helpers it calls for what the comment does not say); rach2/sys/fs/Readme.md, vfs.Readme.md, network/Readme.md; the kernel Readme.


## gettimeofday: number 78, handler sys_gettimeofday

gettimeofday(2): EBX = tv (timeval*), ECX = tz (ignored). Reports real time
from the clock anchored at boot (sys/rtc.c), or time since boot when there
was no clock to read. Returns 0 or -errno.


## clock_gettime: number 265, handler sys_clock_gettime

Read a POSIX clock (clock_gettime, syscall 265, 32-bit timespec).
EBX = clockid, ECX = struct timespec*. CLOCK_REALTIME(0) reports real time
from the clock read at boot (sys/rtc.c); CLOCK_MONOTONIC(1) reports time
since boot, which is what the scheduler, the sleep path and the audio timing
loops mean by a clock. Both run at the 10 ms tick resolution, and neither is
disciplined by anything: the machine reads the CMOS once and then keeps its
own time. musl falls back here from clock_gettime64 (403), which this kernel
does not provide, so times are 32-bit here and this ABI ends in 2038.


## times: number 43, handler sys_times

Process times (times(2)).
Per-process CPU time is not tracked, so the four tms fields are reported
as zero; the return value is the monotonic tick count (already in CLK_TCK
= 100 Hz units). EBX = struct tms * (NULL is allowed: the caller may want
only the return value).


## utimensat: number 320, handler sys_utimensat

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
