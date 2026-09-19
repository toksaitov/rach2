# uname(2)

NAME: uname - the system call family: uname, sethostname, sysinfo.

The page names every member in NAME with .Nm, gives each its prototype in SYNOPSIS as musl declares it (the header and the C prototype; the number in the table is the ABI, EAX), describes the behaviour of this kernel with its differences from Linux where a caller would notice, lists every error the handler returns in ERRORS with when, and ties an example to a probe of the default suite where one exists (tests/*.c). Aliases: every member other than the page name is already listed in man/aliases as an alias of this page; touch no file but the page.

Notes from the milestone: What uname reports (sysname Rach2, the release and version strings); sethostname needs uid 0; sysinfo fields.

Sources: the handler comments below, extracted from rach2/sys/syscall.c (read the handler and the helpers it calls for what the comment does not say); rach2/sys/fs/Readme.md, vfs.Readme.md, network/Readme.md; the kernel Readme.


## uname: number 122, handler sys_uname

(no comment)


## sethostname: number 74, handler sys_sethostname

sethostname(74): set the single mutable host name (uname's nodename, what
gethostname reads back). */


## sysinfo: number 116, handler sys_sysinfo

System statistics (sysinfo(2)).
Fills the 64-byte i386 struct sysinfo: uptime in seconds, a live-process
count, mem_unit = 1; CPU load and memory/swap figures are reported as zero
(not tracked). EBX = struct sysinfo *.
