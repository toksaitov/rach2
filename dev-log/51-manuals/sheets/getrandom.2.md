# getrandom(2)

NAME: getrandom - the system call family: getrandom.

The page names every member in NAME with .Nm, gives each its prototype in SYNOPSIS as musl declares it (the header and the C prototype; the number in the table is the ABI, EAX), describes the behaviour of this kernel with its differences from Linux where a caller would notice, lists every error the handler returns in ERRORS with when, and ties an example to a probe of the default suite where one exists (tests/*.c). Aliases: every member other than the page name is already listed in man/aliases as an alias of this page; touch no file but the page.

Notes from the milestone: The entropy source (RDRAND when trusted, the record `entropy: ready`); flags.

Sources: the handler comments below, extracted from rach2/sys/syscall.c (read the handler and the helpers it calls for what the comment does not say); rach2/sys/fs/Readme.md, vfs.Readme.md, network/Readme.md; the kernel Readme.


## getrandom: number 355, handler sys_getrandom

Fill a buffer with random bytes (getrandom, syscall 355).
EBX = buf, ECX = buflen, EDX = flags. Backed by the kernel CSPRNG
(lib/krandom, ChaCha20). Returns the byte count, like Linux; musl's
getrandom/arc4random sit on top of this. We never block (GRND_NONBLOCK is
a no-op) and have a single source (GRND_RANDOM is equivalent); unknown
flag bits are rejected.
