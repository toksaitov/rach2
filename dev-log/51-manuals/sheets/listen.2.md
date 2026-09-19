# listen(2)

NAME: listen - the socket operation family: listen.

The page names every member in NAME with .Nm, gives each its prototype in SYNOPSIS as musl declares it (the header and the C prototype; the number in the table is the ABI, EAX), describes the behaviour of this kernel with its differences from Linux where a caller would notice, lists every error the handler returns in ERRORS with when, and ties an example to a probe of the default suite where one exists (tests/*.c). Aliases: every member other than the page name is already listed in man/aliases as an alias of this page; touch no file but the page.

Notes from the milestone: The accept array of 16 entries that holds 15 connections and what a full one does (a refusal before the connection is allocated, recorded once; a refusal at handshake completion, recorded each time); ENOBUFS when the heap cannot give a control block.

Sources: the handler comments below, extracted from rach2/sys/syscall.c (read the handler and the helpers it calls for what the comment does not say); rach2/sys/fs/Readme.md, vfs.Readme.md, network/Readme.md; the kernel Readme.


## listen: socketcall 4, handler do_listen

listen(2): mark a stream socket passive and open its LISTEN TCB. */
