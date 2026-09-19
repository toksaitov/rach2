# lseek(2)

NAME: lseek - the system call family: lseek, llseek.

The page names every member in NAME with .Nm, gives each its prototype in SYNOPSIS as musl declares it (the header and the C prototype; the number in the table is the ABI, EAX), describes the behaviour of this kernel with its differences from Linux where a caller would notice, lists every error the handler returns in ERRORS with when, and ties an example to a probe of the default suite where one exists (tests/*.c). Aliases: every member other than the page name goes into man/aliases as `MEMBER 2 lseek`; the writer adds those lines.

Notes from the milestone: Whence values; devices that are not seekable (ESPIPE); the 64-bit form for the disk windows.

Sources: the handler comments below, extracted from rach2/sys/syscall.c (read the handler and the helpers it calls for what the comment does not say); rach2/sys/fs/Readme.md, vfs.Readme.md, network/Readme.md; the kernel Readme.


## lseek: number 19, handler sys_lseek

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


## llseek: number 140, handler sys__llseek

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
