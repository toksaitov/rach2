# ioctl(2)

NAME: ioctl - the system call family: ioctl.

The page names every member in NAME with .Nm, gives each its prototype in SYNOPSIS as musl declares it (the header and the C prototype; the number in the table is the ABI, EAX), describes the behaviour of this kernel with its differences from Linux where a caller would notice, lists every error the handler returns in ERRORS with when, and ties an example to a probe of the default suite where one exists (tests/*.c). Aliases: every member other than the page name goes into man/aliases as `MEMBER 2 ioctl`; the writer adds those lines.

Notes from the milestone: The requests answered, by device: the terminal (TCGETS, TCSETS and the family, TIOCGPGRP, TIOCSPGRP, TIOCSCTTY, TIOCGWINSZ), the disk nodes (BLKRRPART), the mouse and framebuffer if any; ENOTTY otherwise.

Sources: the handler comments below, extracted from rach2/sys/syscall.c (read the handler and the helpers it calls for what the comment does not say); rach2/sys/fs/Readme.md, vfs.Readme.md, network/Readme.md; the kernel Readme.


## ioctl: number 54, handler sys_ioctl

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
