/*
 * ia32_fs.c: the i386 forms of the file calls: the stat and statfs families,
 * the 64-bit offsets i386 splits over two registers, directory reads, fcntl,
 * the iovec calls, select and poll, and asynchronous I/O, for the compat table
 * (ia32_systab.c).
 */

#include "ia32.h"
#include "ia32_abi.h"

#include "../arch.h"
#include "../../fs/fd.h"
#include "../../fs/krffs.h"
#include "../../fs/vfs.h"
#include "../../io/epoll.h"
#include "../../io/poll.h"
#include "../../lib/kstring.h"
#include "../../mm/kheap.h"
#include "../../mm/usercopy.h"
#include "../../network/socket/socket.h"
#include "../../proc/process.h"
#include "../../syscall.h"
#include "../../syscall_internal.h"
#include "../uabi.h"

#if RACH2_IA32

/**
 * Call an x86-64 handler with native arguments. Most compat handlers convert an
 * i386 structure into the x86-64 one in kernel memory, run the x86-64 handler
 * on it, and convert what it wrote back: the handler takes only user
 * addresses, so the conversion buffer is opened to its user-copy checks for
 * the call (`user_window_open`), as sendfile opens its buffer to the read and
 * write paths. The handler gets a copy of the frame that is not marked
 * SC_COMPAT, whose registers carry whole 64-bit arguments, a kernel address
 * among them; `in_compat_syscall` still reads the real frame, so a shared
 * reader that picks the i386 layout for an i386 call (`iov_fetch`) does so
 * for the user's structures and for rwv2's retained copy of its vector.
 * The caller owns `win` for the call, a window of its own thread's; a null
 * window opens nothing. Returns the handler's result. The handler may sleep.
 */
long ia32_native(const isr_params_t *params, syscall_fn_t fn, void *win, uint32_t winlen,
                 unsigned long a0, unsigned long a1, unsigned long a2, unsigned long a3,
                 unsigned long a4, unsigned long a5)
{
    isr_params_t p = *params;
    long rc;

    p.error_code = 0; /* SYSCALL_VECTOR with 0: the `syscall` entry's frame */
    p.rdi = a0;
    p.rsi = a1;
    p.rdx = a2;
    p.r10 = a3;
    p.r8 = a4;
    p.r9 = a5;
    if (win) {
        user_window_open(win, winlen);
    }
    rc = fn(&p);
    if (win) {
        user_window_close();
    }
    return rc;
}

/* A user pointer of an i386 call, zero-extended, as Linux's compat_ptr. */
#define UPTR(v) ((unsigned long)(uint32_t)(v))

/* The address of a conversion buffer, as a native argument. */
#define KPTR(p) ((unsigned long)(uintptr_t)(p))

/* The old 16-bit encoding of the device number `dev`, which the kernel
 * keeps in Linux's new encoding (major in bits 8 to 19, minor in bits 0 to 7
 * and 20 to 31), as Linux's old_encode_dev. */
static uint16_t dev16(unsigned long dev)
{
    uint32_t major = (uint32_t)(dev >> 8) & 0xFFFu;
    uint32_t minor = ((uint32_t)dev & 0xFFu) | ((uint32_t)(dev >> 12) & 0xFFF00u);

    return (uint16_t)((major << 8) | (minor & 0xFFu));
}

/*
 * The stat family. Each call runs the x86-64 handler into a struct
 * user_stat of the kernel's and copies the fields the i386 structure has,
 * with Linux's checks: the old 64-byte stat refuses an inode number, a link
 * count or a size its fields cannot hold (cp_compat_stat), the 32-byte one of
 * the oldest calls an inode number or a link count (cp_old_stat), and stat64
 * holds everything (cp_stat64).
 */

/* The stat of `how` for the arguments: 0 stat64, 1 lstat64, 2 fstat64, 3
 * fstatat64, into `st`. 0 or the handler's error. */
static long stat_native(const isr_params_t *params, int how, struct user_stat *st)
{
    unsigned long a0 = UPTR(SC_ARG(params, 0));
    unsigned long a1 = UPTR(SC_ARG(params, 1));

    k_memset(st, 0, sizeof(*st));
    switch (how) {
    case 0:
        return ia32_native(params, sys_stat64, st, sizeof(*st), a0, KPTR(st), 0, 0, 0, 0);
    case 1:
        return ia32_native(params, sys_lstat64, st, sizeof(*st), a0, KPTR(st), 0, 0, 0, 0);
    case 2:
        return ia32_native(params, sys_fstat64, st, sizeof(*st), (unsigned long)(int)a0, KPTR(st),
                           0, 0, 0, 0);
    default:
        return ia32_native(params, sys_fstatat64, st, sizeof(*st), (unsigned long)(int)a0, a1,
                           KPTR(st), SC_ARG(params, 3), 0, 0);
    }
}

/* Copy `st` out as i386's struct stat to the user's `u`. */
static long put_stat(uintptr_t u, const struct user_stat *st)
{
    struct ia32_stat s;

    if (st->st_ino > 0xFFFFFFFFul || st->st_nlink > 0xFFFFul || st->st_size < 0 ||
        st->st_size > 0x7FFFFFFFl) {
        return -EOVERFLOW;
    }
    k_memset(&s, 0, sizeof(s));
    s.st_dev = (uint32_t)st->st_dev;
    s.st_ino = (uint32_t)st->st_ino;
    s.st_mode = (uint16_t)st->st_mode;
    s.st_nlink = (uint16_t)st->st_nlink;
    s.st_uid = IA32_ID16(st->st_uid);
    s.st_gid = IA32_ID16(st->st_gid);
    s.st_rdev = (uint32_t)st->st_rdev;
    s.st_size = (uint32_t)st->st_size;
    s.st_blksize = (uint32_t)st->st_blksize;
    s.st_blocks = (uint32_t)st->st_blocks;
    s.st_atime = (uint32_t)st->st_atime;
    s.st_atime_nsec = (uint32_t)st->st_atime_nsec;
    s.st_mtime = (uint32_t)st->st_mtime;
    s.st_mtime_nsec = (uint32_t)st->st_mtime_nsec;
    s.st_ctime = (uint32_t)st->st_ctime;
    s.st_ctime_nsec = (uint32_t)st->st_ctime_nsec;
    return copy_to_user((void __user *)u, &s, sizeof(s)) != 0 ? -EFAULT : 0;
}

/* Copy `st` out as __old_kernel_stat to the user's `u`; its size is cut to
 * 32 bits, as Linux's x86-64 kernel cuts it. */
static long put_old_stat(uintptr_t u, const struct user_stat *st)
{
    struct ia32_old_stat s;

    if (st->st_ino > 0xFFFFul || st->st_nlink > 0xFFFFul) {
        return -EOVERFLOW;
    }
    k_memset(&s, 0, sizeof(s));
    s.st_dev = dev16(st->st_dev);
    s.st_ino = (uint16_t)st->st_ino;
    s.st_mode = (uint16_t)st->st_mode;
    s.st_nlink = (uint16_t)st->st_nlink;
    s.st_uid = IA32_ID16(st->st_uid);
    s.st_gid = IA32_ID16(st->st_gid);
    s.st_rdev = dev16(st->st_rdev);
    s.st_size = (uint32_t)st->st_size;
    s.st_atime = (uint32_t)st->st_atime;
    s.st_mtime = (uint32_t)st->st_mtime;
    s.st_ctime = (uint32_t)st->st_ctime;
    return copy_to_user((void __user *)u, &s, sizeof(s)) != 0 ? -EFAULT : 0;
}

/* Copy `st` out as i386's struct stat64 to the user's `u`. */
static long put_stat64(uintptr_t u, const struct user_stat *st)
{
    struct ia32_stat64 s;

    k_memset(&s, 0, sizeof(s));
    IA32_PUT64(&s, st_dev, st->st_dev);
    s.st_ino32 = (uint32_t)st->st_ino;
    s.st_mode = st->st_mode;
    s.st_nlink = (uint32_t)st->st_nlink;
    s.st_uid = st->st_uid;
    s.st_gid = st->st_gid;
    IA32_PUT64(&s, st_rdev, st->st_rdev);
    IA32_PUT64(&s, st_size, st->st_size);
    s.st_blksize = (uint32_t)st->st_blksize;
    IA32_PUT64(&s, st_blocks, st->st_blocks);
    s.st_atime = (uint32_t)st->st_atime;
    s.st_atime_nsec = (uint32_t)st->st_atime_nsec;
    s.st_mtime = (uint32_t)st->st_mtime;
    s.st_mtime_nsec = (uint32_t)st->st_mtime_nsec;
    s.st_ctime = (uint32_t)st->st_ctime;
    s.st_ctime_nsec = (uint32_t)st->st_ctime_nsec;
    IA32_PUT64(&s, st_ino, st->st_ino);
    return copy_to_user((void __user *)u, &s, sizeof(s)) != 0 ? -EFAULT : 0;
}

/* The stat of `how` (`stat_native`), written by `put` to the user's buffer
 * in argument `out`. */
static long stat_as(isr_params_t *params, int how, int out,
                    long (*put)(uintptr_t u, const struct user_stat *st))
{
    struct user_stat st;
    long rc = stat_native(params, how, &st);

    if (rc != 0) {
        return rc;
    }
    return put(UPTR(out == 1 ? SC_ARG(params, 1) : SC_ARG(params, 2)), &st);
}

/** oldstat (18): the path and a struct __old_kernel_stat. */
long compat_sys_stat(isr_params_t *params)
{
    return stat_as(params, 0, 1, put_old_stat);
}

/** oldlstat (84): oldstat of a final symbolic link itself. */
long compat_sys_lstat(isr_params_t *params)
{
    return stat_as(params, 1, 1, put_old_stat);
}

/** oldfstat (28): the descriptor and a struct __old_kernel_stat. */
long compat_sys_fstat(isr_params_t *params)
{
    return stat_as(params, 2, 1, put_old_stat);
}

/** stat (106): the path and i386's struct stat, compat_stat. */
long compat_sys_newstat(isr_params_t *params)
{
    return stat_as(params, 0, 1, put_stat);
}

/** lstat (107): stat of a final symbolic link itself. */
long compat_sys_newlstat(isr_params_t *params)
{
    return stat_as(params, 1, 1, put_stat);
}

/** fstat (108): the descriptor and i386's struct stat. */
long compat_sys_newfstat(isr_params_t *params)
{
    return stat_as(params, 2, 1, put_stat);
}

/** stat64 (195): the path and i386's struct stat64. */
long compat_sys_ia32_stat64(isr_params_t *params)
{
    return stat_as(params, 0, 1, put_stat64);
}

/** lstat64 (196): stat64 of a final symbolic link itself. */
long compat_sys_ia32_lstat64(isr_params_t *params)
{
    return stat_as(params, 1, 1, put_stat64);
}

/** fstat64 (197): the descriptor and i386's struct stat64. */
long compat_sys_ia32_fstat64(isr_params_t *params)
{
    return stat_as(params, 2, 1, put_stat64);
}

/** fstatat64 (300): the directory, the path, i386's struct stat64 and the
 * flags. */
long compat_sys_ia32_fstatat64(isr_params_t *params)
{
    return stat_as(params, 3, 2, put_stat64);
}

/*
 * The statfs family: `ksys_statfs` and `ksys_fstatfs` fill the x86-64 struct
 * statfs in kernel memory, which is copied out in i386's layout with Linux's
 * checks (put_compat_statfs, put_compat_statfs64).
 */

/* The statfs of the path in argument 0, or with `byfd` of the descriptor,
 * into `sf`. 0 or the error. */
static long statfs_native(isr_params_t *params, int byfd, struct user_statfs *sf)
{
    long rc;

    k_memset(sf, 0, sizeof(*sf));
    user_window_open(sf, sizeof(*sf));
    if (byfd) {
        rc = ksys_fstatfs((int)SC_ARG(params, 0), (struct user_statfs __user *)sf);
    } else {
        rc = ksys_statfs((const char __user *)UPTR(SC_ARG(params, 0)),
                         (struct user_statfs __user *)sf);
    }
    user_window_close();
    return rc;
}

/* Whether the count `v` of a struct statfs fits 32 bits; -1, a count a file
 * system does not keep, does. */
static int fits32(long v)
{
    return (unsigned long)v <= 0xFFFFFFFFul || v == -1;
}

/* statfs and fstatfs (99, 100): the path or the descriptor, then i386's
 * struct statfs, compat_statfs, whose 32-bit counts refuse a larger one with
 * -EOVERFLOW. */
static long statfs_call(isr_params_t *params, int byfd)
{
    struct user_statfs sf;
    struct ia32_statfs s;
    long rc = statfs_native(params, byfd, &sf);

    if (rc != 0) {
        return rc;
    }
    if (((unsigned long)sf.f_blocks | (unsigned long)sf.f_bfree | (unsigned long)sf.f_bavail |
         (unsigned long)sf.f_bsize | (unsigned long)sf.f_frsize) > 0xFFFFFFFFul ||
        !fits32(sf.f_files) || !fits32(sf.f_ffree)) {
        return -EOVERFLOW;
    }
    k_memset(&s, 0, sizeof(s));
    s.f_type = (uint32_t)sf.f_type;
    s.f_bsize = (uint32_t)sf.f_bsize;
    s.f_blocks = (uint32_t)sf.f_blocks;
    s.f_bfree = (uint32_t)sf.f_bfree;
    s.f_bavail = (uint32_t)sf.f_bavail;
    s.f_files = (uint32_t)sf.f_files;
    s.f_ffree = (uint32_t)sf.f_ffree;
    s.f_fsid[0] = sf.f_fsid[0];
    s.f_fsid[1] = sf.f_fsid[1];
    s.f_namelen = (uint32_t)sf.f_namelen;
    s.f_frsize = (uint32_t)sf.f_frsize;
    s.f_flags = (uint32_t)sf.f_flags;
    return copy_to_user((void __user *)UPTR(SC_ARG(params, 1)), &s, sizeof(s)) != 0 ? -EFAULT : 0;
}

/** statfs (99): the path and i386's struct statfs. */
long compat_sys_statfs(isr_params_t *params)
{
    return statfs_call(params, 0);
}

/** fstatfs (100): the descriptor and i386's struct statfs. */
long compat_sys_fstatfs(isr_params_t *params)
{
    return statfs_call(params, 1);
}

/* statfs64 and fstatfs64 (268, 269): the path or the descriptor, the size of
 * the structure, which must be its 84 bytes (-EINVAL otherwise), and i386's
 * struct statfs64; a block size past 32 bits is -EOVERFLOW. */
static long statfs64_call(isr_params_t *params, int byfd)
{
    struct user_statfs sf;
    struct ia32_statfs64 s;
    long rc;

    if ((uint32_t)SC_ARG(params, 1) != sizeof(s)) {
        return -EINVAL;
    }
    rc = statfs_native(params, byfd, &sf);
    if (rc != 0) {
        return rc;
    }
    if (((unsigned long)sf.f_bsize | (unsigned long)sf.f_frsize) > 0xFFFFFFFFul) {
        return -EOVERFLOW;
    }
    k_memset(&s, 0, sizeof(s));
    s.f_type = (uint32_t)sf.f_type;
    s.f_bsize = (uint32_t)sf.f_bsize;
    IA32_PUT64(&s, f_blocks, sf.f_blocks);
    IA32_PUT64(&s, f_bfree, sf.f_bfree);
    IA32_PUT64(&s, f_bavail, sf.f_bavail);
    IA32_PUT64(&s, f_files, sf.f_files);
    IA32_PUT64(&s, f_ffree, sf.f_ffree);
    s.f_fsid[0] = sf.f_fsid[0];
    s.f_fsid[1] = sf.f_fsid[1];
    s.f_namelen = (uint32_t)sf.f_namelen;
    s.f_frsize = (uint32_t)sf.f_frsize;
    s.f_flags = (uint32_t)sf.f_flags;
    return copy_to_user((void __user *)UPTR(SC_ARG(params, 2)), &s, sizeof(s)) != 0 ? -EFAULT : 0;
}

/** statfs64 (268): the path, the size and i386's struct statfs64. */
long compat_sys_statfs64(isr_params_t *params)
{
    return statfs64_call(params, 0);
}

/** fstatfs64 (269): the descriptor, the size and i386's struct statfs64. */
long compat_sys_fstatfs64(isr_params_t *params)
{
    return statfs64_call(params, 1);
}

/*
 * The calls whose 64-bit offset or length i386 passes in two registers, low
 * word first, as Linux's arch/x86/kernel/sys_ia32.c has them; the i686
 * kernel's arch/i686/sys_ia32.c has the same forms for its own table.
 */

/* Arguments `lo` and `hi` of the frame as the one 64-bit value they hold;
 * SC_ARG pastes its number, so each is given as a literal. */
#define ARG64(p, lo, hi) IA32_JOIN(SC_ARG(p, lo), SC_ARG(p, hi))

/** truncate64 (193): the path, then the length's low and high words. */
long compat_sys_ia32_truncate64(isr_params_t *params)
{
    return ksys_truncate((const char __user *)UPTR(SC_ARG(params, 0)),
                         (long long)ARG64(params, 1, 2));
}

/** ftruncate64 (194): the descriptor, then the length's two words. */
long compat_sys_ia32_ftruncate64(isr_params_t *params)
{
    return ksys_ftruncate((int)SC_ARG(params, 0), (long long)ARG64(params, 1, 2));
}

/** pread64 (180): the descriptor, the buffer, the count, then the offset's
 * two words. The count goes whole to `ksys_pread64`, which checks the end it
 * gives before the cut, as Linux's ia32_pread64 passes its u32. */
long compat_sys_ia32_pread64(isr_params_t *params)
{
    return ksys_pread64((int)SC_ARG(params, 0), (char __user *)UPTR(SC_ARG(params, 1)),
                        SC_ARG(params, 2), (long long)ARG64(params, 3, 4));
}

/** pwrite64 (181): pread64's arguments, the buffer written from. */
long compat_sys_ia32_pwrite64(isr_params_t *params)
{
    return ksys_pwrite64((int)SC_ARG(params, 0), (const char __user *)UPTR(SC_ARG(params, 1)),
                         SC_ARG(params, 2), (long long)ARG64(params, 3, 4));
}

/** fallocate (324): the descriptor, the mode, then the offset and the length
 * in two words each. */
long compat_sys_ia32_fallocate(isr_params_t *params)
{
    return ksys_fallocate((int)SC_ARG(params, 0), (uint32_t)SC_ARG(params, 1), ARG64(params, 2, 3),
                          ARG64(params, 4, 5));
}

/** fadvise64 (250): the descriptor, the offset in two words, a 32-bit
 * length, never negative, and the advice. */
long compat_sys_ia32_fadvise64(isr_params_t *params)
{
    return ksys_fadvise64((int)SC_ARG(params, 0), (long long)(uint32_t)SC_ARG(params, 3),
                          (uint32_t)SC_ARG(params, 4));
}

/** fadvise64_64 (272): the descriptor, the offset and the length in two words
 * each, and the advice. */
long compat_sys_ia32_fadvise64_64(isr_params_t *params)
{
    return ksys_fadvise64((int)SC_ARG(params, 0), (long long)ARG64(params, 3, 4),
                          (uint32_t)SC_ARG(params, 5));
}

/** readahead (225): the descriptor, the offset in two words and the count. */
long compat_sys_ia32_readahead(isr_params_t *params)
{
    return ia32_native(params, sys_readahead, NULL, 0, (unsigned long)(int)SC_ARG(params, 0),
                       (unsigned long)ARG64(params, 1, 2), SC_ARG(params, 3), 0, 0, 0);
}

/** Join sync_file_range's split offset and length before the shared call. */
long compat_sys_ia32_sync_file_range(isr_params_t *params)
{
    return ksys_sync_file_range((int)SC_ARG(params, 0), (int64_t)ARG64(params, 1, 2),
                                (int64_t)ARG64(params, 3, 4), (uint32_t)SC_ARG(params, 5));
}

/** Read i386 getdents records while holding the directory description. */
long compat_sys_getdents(isr_params_t *params)
{
    return ksys_getdents32((int)SC_ARG(params, 0), UPTR(SC_ARG(params, 1)),
                           (uint32_t)SC_ARG(params, 2), 0);
}

/** Read one old i386 directory record; Linux ignores the count. */
long compat_sys_old_readdir(isr_params_t *params)
{
    return ksys_getdents32((int)SC_ARG(params, 0), UPTR(SC_ARG(params, 1)), 0, 1);
}

/*
 * fcntl and fcntl64 (55, 221), which i386 both answers with Linux's
 * compat_sys_fcntl64. The lock commands take i386's struct flock, 32-bit
 * offsets, for F_GETLK, F_SETLK and F_SETLKW, and struct flock64, packed, for
 * the 64-bit and open file description ones; each is converted to the x86-64
 * struct flock, for the x86-64 command of the same lock, and F_GETLK's
 * answer back. The other commands pass as they are.
 */

#define F_GETLK      5
#define F_SETLK      6
#define F_SETLKW     7
#define F_GETLK64    12
#define F_SETLK64    13
#define F_SETLKW64   14
#define F_OFD_GETLK  36
#define F_OFD_SETLK  37
#define F_OFD_SETLKW 38

/* The x86-64 struct flock. */
struct native_flock {
    int16_t l_type;
    int16_t l_whence;
    int64_t l_start;
    int64_t l_len;
    int32_t l_pid;
};

/** fcntl64 (221), and fcntl (55): the descriptor, the command and its
 * argument. A 32-bit lock F_GETLK reports at an offset past 0x7FFFFFFF is
 * -EOVERFLOW, and its length is cut to 0x7FFFFFFF, as fixup_compat_flock
 * has it. */
long compat_sys_fcntl64(isr_params_t *params)
{
    int fd = (int)SC_ARG(params, 0);
    int cmd = (int)SC_ARG(params, 1);
    uintptr_t uarg = UPTR(SC_ARG(params, 2));
    struct native_flock k;
    int narrow, ncmd;
    long rc;

    switch (cmd) {
    case F_GETLK:
    case F_SETLK:
    case F_SETLKW:
        narrow = 1;
        ncmd = cmd - F_GETLK + F_GETLK64;
        break;
    case F_GETLK64:
    case F_SETLK64:
    case F_SETLKW64:
    case F_OFD_GETLK:
    case F_OFD_SETLK:
    case F_OFD_SETLKW:
        narrow = 0;
        ncmd = cmd;
        break;
    default:
        return ia32_native(params, sys_fcntl64, NULL, 0, (unsigned long)fd, (unsigned long)cmd,
                           SC_ARG(params, 2), 0, 0, 0);
    }
    k_memset(&k, 0, sizeof(k));
    if (narrow) {
        struct ia32_flock f;

        if (copy_from_user(&f, (const void __user *)uarg, sizeof(f)) != 0) {
            return -EFAULT;
        }
        k.l_type = f.l_type;
        k.l_whence = f.l_whence;
        k.l_start = f.l_start;
        k.l_len = f.l_len;
        k.l_pid = f.l_pid;
    } else {
        struct ia32_flock64 f;

        if (copy_from_user(&f, (const void __user *)uarg, sizeof(f)) != 0) {
            return -EFAULT;
        }
        k.l_type = f.l_type;
        k.l_whence = f.l_whence;
        k.l_start = (int64_t)IA32_GET64(&f, l_start);
        k.l_len = (int64_t)IA32_GET64(&f, l_len);
        k.l_pid = f.l_pid;
    }
    rc = ia32_native(params, sys_fcntl64, &k, sizeof(k), (unsigned long)fd, (unsigned long)ncmd,
                     KPTR(&k), 0, 0, 0);
    if (rc != 0 || (cmd != F_GETLK && cmd != F_GETLK64 && cmd != F_OFD_GETLK)) {
        return rc;
    }
    if (narrow) {
        struct ia32_flock f;

        if (k.l_start > 0x7FFFFFFF) {
            return -EOVERFLOW;
        }
        f.l_type = k.l_type;
        f.l_whence = k.l_whence;
        f.l_start = (int32_t)k.l_start;
        f.l_len = (int32_t)(k.l_len > 0x7FFFFFFF ? 0x7FFFFFFF : k.l_len);
        f.l_pid = k.l_pid;
        return copy_to_user((void __user *)uarg, &f, sizeof(f)) != 0 ? -EFAULT : 0;
    } else {
        struct ia32_flock64 f;

        f.l_type = k.l_type;
        f.l_whence = k.l_whence;
        IA32_PUT64(&f, l_start, k.l_start);
        IA32_PUT64(&f, l_len, k.l_len);
        f.l_pid = k.l_pid;
        return copy_to_user((void __user *)uarg, &f, sizeof(f)) != 0 ? -EFAULT : 0;
    }
}

/*
 * The vector calls with a position: preadv and pwritev, and preadv2 and
 * pwritev2, whose position -1 is the descriptor's own offset. The vector is
 * the user's, read in i386's layout by the shared reader (`iov_fetch`).
 */

/** preadv (333): the descriptor, the vector, its count and the position in
 * two words. */
long compat_sys_preadv(isr_params_t *params)
{
    return ia32_native(params, sys_preadv, NULL, 0, (unsigned long)(int)SC_ARG(params, 0),
                       UPTR(SC_ARG(params, 1)), SC_ARG(params, 2),
                       (unsigned long)ARG64(params, 3, 4), 0, 0);
}

/** pwritev (334): preadv's arguments, the vector written from. */
long compat_sys_pwritev(isr_params_t *params)
{
    return ia32_native(params, sys_pwritev, NULL, 0, (unsigned long)(int)SC_ARG(params, 0),
                       UPTR(SC_ARG(params, 1)), SC_ARG(params, 2),
                       (unsigned long)ARG64(params, 3, 4), 0, 0);
}

/**
 * Join the split offset of preadv2 or pwritev2 before the native call.
 * `write` selects pwritev2; otherwise read. The caller holds the big lock.
 * Returns the native handler's count or negative error; it may sleep.
 */
static long rw2(isr_params_t *params, int write)
{
    return ia32_native(params, write ? sys_pwritev2 : sys_preadv2, NULL, 0,
                       (unsigned long)(int)SC_ARG(params, 0), UPTR(SC_ARG(params, 1)),
                       SC_ARG(params, 2), (unsigned long)ARG64(params, 3, 4), 0,
                       (uint32_t)SC_ARG(params, 5));
}

/** preadv2 (378): preadv's arguments and the flags. */
long compat_sys_preadv2(isr_params_t *params)
{
    return rw2(params, 0);
}

/** pwritev2 (379): pwritev's arguments and the flags. */
long compat_sys_pwritev2(isr_params_t *params)
{
    return rw2(params, 1);
}

/*
 * select and poll. The fd_sets of i386 are arrays of 32-bit longs, so a set
 * is read and written for the 32-bit words that hold its first `nfds` bits,
 * where the x86-64 handler moves whole 64-bit longs. The sets are copied into
 * one kernel buffer, each rounded up to a 64-bit long with zero bits, with
 * the timeout beside them, the x86-64 handler runs on it, and the sets are
 * copied back at their i386 size. The timeouts are converted to the x86-64
 * timeval and timespec; a 64-bit timespec of the _time64 calls has the high
 * word of its nanoseconds ignored, as Linux's get_timespec64 ignores it for
 * an i386 caller.
 */

/* The conversion buffer of a select: the timeout, the sixth argument of
 * pselect6, and the three sets of the x86-64 handler, of longs. */
struct sel_buf {
    long ts[2];
    unsigned long sigarg[2];
    unsigned long words[1]; /* the three sets, `kbytes` bytes each */
};

/* Read the 64-bit timespec at `u` into `ts`, the high word of its
 * nanoseconds dropped. 0 or -EFAULT. */
int ia32_get_timespec64(uintptr_t u, long ts[2])
{
    struct ia32_timespec64 t;

    if (copy_from_user(&t, (const void __user *)u, sizeof(t)) != 0) {
        return -EFAULT;
    }
    ts[0] = (long)IA32_GET64(&t, tv_sec);
    ts[1] = (long)t.tv_nsec_lo;
    return 0;
}

/* Read the 32-bit timespec at `u` into `ts`. 0 or -EFAULT. */
int ia32_get_timespec(uintptr_t u, long ts[2])
{
    struct ia32_timespec t;

    if (copy_from_user(&t, (const void __user *)u, sizeof(t)) != 0) {
        return -EFAULT;
    }
    ts[0] = t.tv_sec;
    ts[1] = t.tv_nsec;
    return 0;
}

/* The kinds of timeout of the select calls. */
enum { SEL_TIMEVAL, SEL_TIMESPEC, SEL_TIMESPEC64 };

/** Whether the timeout `ts`, of `kind`, is zero after normalization.
 * A timeval's fields came from signed 32-bit words, so its sum fits. */
static int select_timeout_zero(int kind, const long ts[2])
{
    if (kind == SEL_TIMEVAL) {
        return ts[0] + ts[1] / 1000000L == 0 && ts[1] % 1000000L == 0;
    }
    return ts[0] == 0 && ts[1] == 0;
}

/** Give the time left, which the native call wrote to `ts`, back to the
 * timeout at `ut`, of the kind `kind`, unless it was `zero`, as
 * `poll_left_out` does; returns `rc`, the call's result. Here the native
 * call has nonnegative nfds and kernel-backed sets, so EINVAL or EFAULT
 * means its timeout or signal-mask validation failed before the wait. */
static long select_left_out(long rc, uintptr_t ut, int kind, int zero, const long ts[2])
{
    int form = kind == SEL_TIMEVAL ? POLL_LEFT_USEC : kind == SEL_TIMESPEC64 ? POLL_LEFT_WIDE : 0;

    if (rc == -EINVAL || rc == -EFAULT) {
        return rc;
    }
    return poll_left_out((int32_t)rc, ut, form, zero, ts[0],
                         kind == SEL_TIMEVAL ? ts[1] * 1000L : ts[1]);
}

/**
 * The body of the select calls: the descriptors below `nfds` set in the
 * i386 fd_sets at `ur`, `uw` and `ue`, each 0 for none, the timeout at `ut`
 * of the kind `kind`, 0 for none, and for pselect6 (`usig` not 0) the
 * address of its sixth argument. The bits are counted as the x86-64 handler
 * counts them: `nfds` is cut to the size of the descriptor table, at least
 * 64 (`select_common` in io/poll.c). The time left goes back to the
 * timeout in its own form (`poll_left_out`). Returns as the x86-64 handler
 * does, -EFAULT for a set, timeout or argument that cannot be read or
 * written.
 */
static long select_body(isr_params_t *params, long nfds, uintptr_t ur, uintptr_t uw, uintptr_t ue,
                        uintptr_t ut, int kind, uintptr_t usig)
{
    uintptr_t u[3];
    struct sel_buf *b;
    uint32_t ubytes, kbytes, size;
    unsigned long kset[3], kts = 0, ksig = 0;
    long cap, rc;
    int i;

    if (!current_proc) {
        return -ESRCH;
    }
    if (nfds < 0) {
        return -EINVAL;
    }
    cap = fd_table_cap(current_proc->fds);
    if (cap < 64) {
        cap = 64;
    }
    if (nfds > cap) {
        nfds = cap;
    }
    ubytes = ((uint32_t)nfds + 31u) / 32u * 4u;
    kbytes = ((uint32_t)nfds + 63u) / 64u * 8u;
    size = (uint32_t)sizeof(*b) + 3u * kbytes;
    b = kzalloc(size);
    if (!b) {
        return -ENOMEM;
    }
    u[0] = ur;
    u[1] = uw;
    u[2] = ue;
    rc = 0;
    for (i = 0; i < 3; i++) {
        uint8_t *set = (uint8_t *)b->words + (uint32_t)i * kbytes;

        kset[i] = u[i] ? KPTR(set) : 0;
        if (u[i] && copy_from_user(set, (const void __user *)u[i], ubytes) != 0) {
            rc = -EFAULT;
        }
    }
    if (rc == 0 && ut) {
        if (kind == SEL_TIMEVAL) {
            struct ia32_timeval tv = {0, 0};

            if (copy_from_user(&tv, (const void __user *)ut, sizeof(tv)) != 0) {
                rc = -EFAULT;
            }
            b->ts[0] = tv.tv_sec;
            b->ts[1] = tv.tv_usec;
        } else {
            rc = kind == SEL_TIMESPEC ? ia32_get_timespec(ut, b->ts)
                                      : ia32_get_timespec64(ut, b->ts);
        }
        kts = KPTR(b->ts);
    }
    if (rc == 0 && usig) {
        struct ia32_sigset_arg sa = {0, 0};

        if (copy_from_user(&sa, (const void __user *)usig, sizeof(sa)) != 0) {
            rc = -EFAULT;
        }
        b->sigarg[0] = sa.ss;
        b->sigarg[1] = sa.ss_len;
        ksig = KPTR(b->sigarg);
    }
    if (rc == 0) {
        int zero = select_timeout_zero(kind, b->ts);

        rc = ia32_native(params, kind == SEL_TIMEVAL ? sys__newselect : sys_pselect6, b, size,
                         (unsigned long)nfds, kset[0], kset[1], kset[2], kts, ksig);
        rc = select_left_out(rc, ut, kind, zero, b->ts);
    }
    for (i = 0; rc >= 0 && i < 3; i++) {
        const uint8_t *set = (const uint8_t *)b->words + (uint32_t)i * kbytes;

        if (u[i] && copy_to_user((void __user *)u[i], set, ubytes) != 0) {
            rc = -EFAULT;
        }
    }
    kfree(b);
    return rc;
}

/** select (82), the old one: the address of a sel_arg_struct holding the
 * five arguments of _newselect. */
long compat_sys_old_select(isr_params_t *params)
{
    struct ia32_sel_arg a;

    if (copy_from_user(&a, (const void __user *)UPTR(SC_ARG(params, 0)), sizeof(a)) != 0) {
        return -EFAULT;
    }
    return select_body(params, (int32_t)a.n, a.inp, a.outp, a.exp, a.tvp, SEL_TIMEVAL, 0);
}

/** _newselect (142): nfds, the three fd_sets and an i386 timeval. */
long compat_sys_select(isr_params_t *params)
{
    return select_body(params, (int32_t)SC_ARG(params, 0), UPTR(SC_ARG(params, 1)),
                       UPTR(SC_ARG(params, 2)), UPTR(SC_ARG(params, 3)), UPTR(SC_ARG(params, 4)),
                       SEL_TIMEVAL, 0);
}

/* pselect6 with the timeout of `kind`: nfds, the three sets, the timeout and
 * the address of the mask's address and size. */
static long pselect6_kind(isr_params_t *params, int kind)
{
    return select_body(params, (int32_t)SC_ARG(params, 0), UPTR(SC_ARG(params, 1)),
                       UPTR(SC_ARG(params, 2)), UPTR(SC_ARG(params, 3)), UPTR(SC_ARG(params, 4)),
                       kind, UPTR(SC_ARG(params, 5)));
}

/** pselect6 (308): its timeout an i386 timespec. */
long compat_sys_pselect6_time32(isr_params_t *params)
{
    return pselect6_kind(params, SEL_TIMESPEC);
}

/** pselect6_time64 (413): its timeout a 64-bit timespec. */
long compat_sys_pselect6_time64(isr_params_t *params)
{
    return pselect6_kind(params, SEL_TIMESPEC64);
}

/* ppoll with the timeout of `kind`: the pollfds, which have one layout in
 * both, their count, the timeout, the mask and its size. */
static long ppoll_kind(isr_params_t *params, int kind)
{
    long ts[2], ret;
    uintptr_t ut = UPTR(SC_ARG(params, 2));
    int rc, zero;

    if (!ut) {
        return ia32_native(params, sys_ppoll, NULL, 0, UPTR(SC_ARG(params, 0)), SC_ARG(params, 1),
                           0, UPTR(SC_ARG(params, 3)), SC_ARG(params, 4), 0);
    }
    rc = kind == SEL_TIMESPEC ? ia32_get_timespec(ut, ts) : ia32_get_timespec64(ut, ts);
    if (rc != 0) {
        return rc;
    }
    zero = ts[0] == 0 && ts[1] == 0;
    ret = ia32_native(params, sys_ppoll, ts, sizeof(ts), UPTR(SC_ARG(params, 0)), SC_ARG(params, 1),
                      KPTR(ts), UPTR(SC_ARG(params, 3)), SC_ARG(params, 4), 0);
    /* The time left the native call wrote to the copy goes back. */
    return poll_left_out((int32_t)ret, ut, kind == SEL_TIMESPEC64 ? POLL_LEFT_WIDE : 0, zero, ts[0],
                         ts[1]);
}

/** ppoll (309): its timeout an i386 timespec. */
long compat_sys_ppoll_time32(isr_params_t *params)
{
    return ppoll_kind(params, SEL_TIMESPEC);
}

/** ppoll_time64 (414): its timeout a 64-bit timespec. */
long compat_sys_ppoll_time64(isr_params_t *params)
{
    return ppoll_kind(params, SEL_TIMESPEC64);
}

/** epoll_pwait2 (441): epoll_pwait's arguments with a 64-bit timespec in
 * place of the milliseconds, its high word of nanoseconds ignored; the
 * events and the mask have one layout in both. */
long compat_sys_epoll_pwait2(isr_params_t *params)
{
    long ts[2];
    uintptr_t ut = UPTR(SC_ARG(params, 3));
    int rc;

    if (ut) {
        rc = ia32_get_timespec64(ut, ts);
        if (rc != 0) {
            return rc;
        }
    }
    return ia32_native(params, sys_epoll_pwait2, ut ? ts : NULL, sizeof(ts),
                       (unsigned long)(int)SC_ARG(params, 0), UPTR(SC_ARG(params, 1)),
                       (unsigned long)(int)SC_ARG(params, 2), ut ? KPTR(ts) : 0,
                       UPTR(SC_ARG(params, 4)), SC_ARG(params, 5));
}

/** ustat (62): the device number and i386's struct ustat, whose counts are
 * truncated to 32 bits as Linux's compat_sys_ustat truncates them. */
long compat_sys_ustat(isr_params_t *params)
{
    struct ia32_ustat u;
    uint64_t bfree;
    uint64_t ffree;
    int rc = ksys_ustat_counts((uint32_t)SC_ARG(params, 0), &bfree, &ffree);

    if (rc != 0) {
        return rc;
    }
    k_memset(&u, 0, sizeof(u));
    u.f_tfree = (int32_t)bfree;
    u.f_tinode = (uint32_t)ffree;
    return copy_to_user((void __user *)UPTR(SC_ARG(params, 1)), &u, sizeof(u)) != 0 ? -EFAULT : 0;
}

/* Each answers ENOSYS until its translation is written (ia32.h). */
IA32_STUB(compat_sys_io_setup)
IA32_STUB(compat_sys_io_getevents_time32)
IA32_STUB(compat_sys_io_submit)
IA32_STUB(compat_sys_fanotify_mark)
IA32_STUB(compat_sys_io_pgetevents)
IA32_STUB(compat_sys_io_pgetevents_time64)

#endif /* RACH2_IA32 */
