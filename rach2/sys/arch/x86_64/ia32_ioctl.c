/*
 * ia32_ioctl.c: the compat ioctl path, which passes a command whose argument
 * has one layout in both ABIs and translates or refuses the others, for the
 * compat table (ia32_systab.c).
 */

#include "ia32.h"

#include "../../fs/fd.h"
#include "../../drm/drm.h"
#include "../../lib/kstring.h"
#include "../../mm/kheap.h"
#include "../../mm/usercopy.h"
#include "../../network/driver/netdev.h"
#include "../../network/socket/socket.h"
#include "../../proc/process.h"
#include "../../syscall.h"

#if RACH2_IA32

/* The fields of an ioctl number, Linux's asm-generic encoding: 2 bits of
 * direction from bit 30, 14 of size from bit 16, 8 of type, 8 of number. */
#define IOC_DIR(c)  (((c) >> 30) & 3u)
#define IOC_SIZE(c) (((c) >> 16) & 0x3FFFu)
#define IOC_TYPE(c) (((c) >> 8) & 0xFFu)
#define IOC_NR(c)   ((c) & 0xFFu)
#define IOC_READ    2u

/* The number `c` with the size `size` in place of its own. */
#define IOC_RESIZE(c, size) (((c) & ~(0x3FFFu << 16)) | ((uint32_t)(size) << 16))

long ia32_ioctl_native(unsigned int fd, unsigned int cmd, void *buf, uint32_t len)
{
    long rc;

    user_window_open(buf, len);
    rc = ioctl_fd((int)fd, cmd, (uintptr_t)buf);
    user_window_close();
    return rc;
}

/** The native request `cmd` on `fd` with the caller's pointer `arg`, for a
 * command whose argument has one layout in both ABIs. */
static long pass(unsigned int fd, unsigned int cmd, unsigned long arg)
{
    return ioctl_fd((int)fd, cmd, (uintptr_t)arg);
}

/* ------------------------------------------------------------------------
 * File attributes: chattr(1)'s requests are numbered with a `long`, 4 bytes
 * on i386, and take an int in both ABIs, as Linux's compat_sys_ioctl and
 * ext4's compat handler take them.
 */

#define FS_IOC32_GETFLAGS   0x80046601u
#define FS_IOC32_SETFLAGS   0x40046602u
#define FS_IOC32_GETVERSION 0x80047601u
#define FS_IOC32_SETVERSION 0x40047602u

/** The file attribute requests of types 'f' and 'v': the four numbered with
 * i386's `long` become the native ones; every other passes. */
static long fs_attr(const fd_entry_t *e, unsigned int fd, unsigned int cmd, unsigned long arg)
{
    (void)e;
    if (cmd == FS_IOC32_GETFLAGS || cmd == FS_IOC32_SETFLAGS || cmd == FS_IOC32_GETVERSION ||
        cmd == FS_IOC32_SETVERSION) {
        cmd = IOC_RESIZE(cmd, sizeof(long));
    }
    return pass(fd, cmd, arg);
}

/* ------------------------------------------------------------------------
 * Block devices: the requests of a `long`, which i386 takes as 4 bytes, and
 * HDIO_GETGEO's `hd_geometry`, whose start is a `long`, as Linux's
 * compat_blkdev_ioctl translates them. BLKBSZGET and BLKGETSIZE64, which are
 * numbered with a `size_t`, take both numbers natively.
 */

#define BLKGETSIZE  0x1260u
#define BLKRAGET    0x1263u
#define BLKFRAGET   0x1265u
#define HDIO_GETGEO 0x0301u

/** BLKGETSIZE, BLKRAGET and BLKFRAGET in a 32-bit word, -EFBIG for a sector
 * count past it and -EINVAL for the read-ahead into a null pointer, as Linux
 * answers them; every other request of type 0x12 passes. The native request
 * answers first, so that a descriptor of another kind gives its own answer,
 * as a file with no block device's compat handler does on Linux. */
static long blk(const fd_entry_t *e, unsigned int fd, unsigned int cmd, unsigned long arg)
{
    uint64_t v = 0;
    uint32_t v32;
    long rc;

    (void)e;
    if (cmd != BLKGETSIZE && cmd != BLKRAGET && cmd != BLKFRAGET) {
        return pass(fd, cmd, arg);
    }
    rc = ia32_ioctl_native(fd, cmd, &v, sizeof(v));
    if (rc != 0) {
        return rc;
    }
    if (cmd != BLKGETSIZE && arg == 0) {
        return -EINVAL;
    }
    if (v > 0xFFFFFFFFu) {
        return -EFBIG;
    }
    v32 = (uint32_t)v;
    return copy_to_user((void __user *)arg, &v32, sizeof(v32)) != 0 ? -EFAULT : 0;
}

/** HDIO_GETGEO: Linux's `hd_geometry` of 8 bytes, the start cut to 32 bits
 * as Linux cuts it, -EINVAL for a null pointer; another request of type 3,
 * which Linux translates and Rach2 does not answer, is -ENOTTY. */
static long hdio(const fd_entry_t *e, unsigned int fd, unsigned int cmd, unsigned long arg)
{
    struct {
        uint8_t heads;
        uint8_t sectors;
        uint16_t cylinders;
        unsigned long start;
    } geo;
    uint8_t geo32[8];
    uint32_t start;
    long rc;

    (void)e;
    if (cmd != HDIO_GETGEO) {
        return -ENOTTY;
    }
    k_memset(&geo, 0, sizeof(geo));
    rc = ia32_ioctl_native(fd, cmd, &geo, sizeof(geo));
    if (rc != 0) {
        return rc;
    }
    if (arg == 0) {
        return -EINVAL;
    }
    start = (uint32_t)geo.start;
    k_memcpy(geo32, &geo, 4);
    k_memcpy(geo32 + 4, &start, 4);
    return copy_to_user((void __user *)arg, geo32, sizeof(geo32)) != 0 ? -EFAULT : 0;
}

/* ------------------------------------------------------------------------
 * Loop devices: LOOP_SET_STATUS and LOOP_GET_STATUS with i386's `loop_info`,
 * which Linux's lo_compat_ioctl takes through `loop_info64`, the structure
 * LOOP_SET_STATUS64 and LOOP_GET_STATUS64 pass in both ABIs.
 */

#define LOOP_SET_STATUS   0x4C02u
#define LOOP_GET_STATUS   0x4C03u
#define LOOP_SET_STATUS64 0x4C04u
#define LOOP_GET_STATUS64 0x4C05u

/* i386's `struct loop_info`, Linux's compat_loop_info: 140 bytes, with
 * 16-bit device numbers and a 32-bit inode. */
struct loop_info32 {
    int32_t number;
    uint16_t device;
    uint16_t pad1;
    uint32_t inode;
    uint16_t rdevice;
    uint16_t pad2;
    int32_t offset;
    int32_t encrypt_type;
    int32_t encrypt_key_size;
    int32_t flags;
    char name[64];
    uint8_t encrypt_key[32];
    uint32_t init[2];
    char reserved[4];
};

/* `struct loop_info64`, 232 bytes in both ABIs. */
struct loop_info64 {
    uint64_t device;
    uint64_t inode;
    uint64_t rdevice;
    uint64_t offset;
    uint64_t sizelimit;
    uint32_t number;
    uint32_t encrypt_type;
    uint32_t encrypt_key_size;
    uint32_t flags;
    char file_name[64];
    char crypt_name[64];
    uint8_t encrypt_key[32];
    uint64_t init[2];
};

/** LOOP_SET_STATUS and LOOP_GET_STATUS by their 64-bit forms, as Linux's
 * loop_info64_from_compat and loop_info64_to_compat convert them: what the
 * old structure has room for, -EOVERFLOW for a value it has not, and
 * -EINVAL for LOOP_GET_STATUS of a null pointer. Every other request of
 * type 'L' passes. */
static long loop(const fd_entry_t *e, unsigned int fd, unsigned int cmd, unsigned long arg)
{
    struct loop_info32 i32;
    struct loop_info64 i64;
    long rc;

    (void)e;
    k_memset(&i64, 0, sizeof(i64));
    if (cmd == LOOP_SET_STATUS) {
        if (copy_from_user(&i32, (const void __user *)arg, sizeof(i32)) != 0) {
            return -EFAULT;
        }
        i64.number = (uint32_t)i32.number;
        i64.device = i32.device;
        i64.inode = i32.inode;
        i64.rdevice = i32.rdevice;
        i64.offset = (uint64_t)(int64_t)i32.offset;
        i64.flags = (uint32_t)i32.flags;
        k_memcpy(i64.file_name, i32.name, sizeof(i32.name));
        return ia32_ioctl_native(fd, LOOP_SET_STATUS64, &i64, sizeof(i64));
    }
    if (cmd != LOOP_GET_STATUS) {
        return pass(fd, cmd, arg);
    }
    if (arg == 0) {
        return -EINVAL;
    }
    rc = ia32_ioctl_native(fd, LOOP_GET_STATUS64, &i64, sizeof(i64));
    if (rc != 0) {
        return rc;
    }
    k_memset(&i32, 0, sizeof(i32));
    i32.number = (int32_t)i64.number;
    i32.device = (uint16_t)i64.device;
    i32.inode = (uint32_t)i64.inode;
    i32.rdevice = (uint16_t)i64.rdevice;
    i32.offset = (int32_t)i64.offset;
    i32.flags = (int32_t)i64.flags;
    k_memcpy(i32.name, i64.file_name, sizeof(i32.name));
    if (i32.device != i64.device || i32.rdevice != i64.rdevice || i32.inode != i64.inode ||
        (int64_t)i32.offset != (int64_t)i64.offset) {
        return -EOVERFLOW;
    }
    return copy_to_user((void __user *)arg, &i32, sizeof(i32)) != 0 ? -EFAULT : 0;
}

/* ------------------------------------------------------------------------
 * evdev: the bit sets, which Linux gives a compat caller in 32-bit longs, so
 * a set of fewer than 33 bits is 4 bytes and not 8 (evdev.c's bits_to_user).
 */

#define EV_NR_GBIT   0x20u /* EVIOCGBIT(type) is this plus the type */
#define EV_NR_GPROP  0x09u
#define EV_NR_GKEY   0x18u
#define EV_NR_GLED   0x19u
#define EV_NR_GSND   0x1Au
#define EV_NR_GSW    0x1Bu
#define EV_BITS_MAX  128u /* the most bytes a set of evdev's takes, KEY_MAX's */
#define EV_COMPAT(m) (((m) - 1u) / 32u * 4u + 4u)

/** The bytes Linux gives a compat caller of the bit set the read request
 * numbered `nr` reads, from the set's highest bit, or 0 for a request that
 * reads no set. */
static uint32_t ev_compat_bytes(uint32_t nr)
{
    switch (nr) {
    case EV_NR_GPROP:
        return EV_COMPAT(0x1Fu); /* INPUT_PROP_MAX */
    case EV_NR_GKEY:
        return EV_COMPAT(0x2FFu); /* KEY_MAX */
    case EV_NR_GLED:
        return EV_COMPAT(0x0Fu); /* LED_MAX */
    case EV_NR_GSND:
        return EV_COMPAT(0x07u); /* SND_MAX */
    case EV_NR_GSW:
        return EV_COMPAT(0x10u); /* SW_MAX */
    case EV_NR_GBIT + 0x00u:
        return EV_COMPAT(0x1Fu); /* EV_MAX */
    case EV_NR_GBIT + 0x01u:
        return EV_COMPAT(0x2FFu); /* EV_KEY */
    case EV_NR_GBIT + 0x02u:
        return EV_COMPAT(0x0Fu); /* EV_REL */
    case EV_NR_GBIT + 0x03u:
        return EV_COMPAT(0x3Fu); /* EV_ABS */
    case EV_NR_GBIT + 0x04u:
        return EV_COMPAT(0x07u); /* EV_MSC */
    case EV_NR_GBIT + 0x05u:
        return EV_COMPAT(0x10u); /* EV_SW */
    case EV_NR_GBIT + 0x11u:
        return EV_COMPAT(0x0Fu); /* EV_LED */
    case EV_NR_GBIT + 0x12u:
        return EV_COMPAT(0x07u); /* EV_SND */
    case EV_NR_GBIT + 0x15u:
        return EV_COMPAT(0x7Fu); /* EV_FF */
    default:
        return 0;
    }
}

/** The bit set reads of type 'E', cut to their compat length: the native
 * request reads the set into a buffer here, and the caller gets as much of
 * it as Linux gives a compat caller, which is also the result. Every other
 * request of type 'E' passes, EVIOCSFF among them, which evdev does not
 * answer. */
static long evdev(const fd_entry_t *e, unsigned int fd, unsigned int cmd, unsigned long arg)
{
    uint8_t bits[EV_BITS_MAX];
    uint32_t want = ev_compat_bytes(IOC_NR(cmd));
    uint32_t len = IOC_SIZE(cmd);
    long rc;

    (void)e;
    if (IOC_DIR(cmd) != IOC_READ || want == 0) {
        return pass(fd, cmd, arg);
    }
    if (len > sizeof(bits)) {
        len = sizeof(bits);
    }
    rc = ia32_ioctl_native(fd, IOC_RESIZE(cmd, len), bits, len);
    if (rc < 0) {
        return rc;
    }
    if ((uint32_t)rc > want) {
        rc = (long)want;
    }
    return copy_to_user((void __user *)arg, bits, (size_t)rc) != 0 ? -EFAULT : rc;
}

/* ------------------------------------------------------------------------
 * The console's sound requests: AUDIO_WRITE's argument holds a pointer.
 */

#define AUDIO_WRITE 0x4104u

/** AUDIO_WRITE with its i386 argument, a 32-bit pointer to the samples and
 * their length; every other request of type 'A' passes. */
static long audio(const fd_entry_t *e, unsigned int fd, unsigned int cmd, unsigned long arg)
{
    uint32_t w32[2];
    struct {
        uintptr_t buffer;
        uint32_t size;
    } w;

    if (cmd != AUDIO_WRITE || e->type != FD_TYPE_CONSOLE) {
        return pass(fd, cmd, arg);
    }
    if (arg == 0 || copy_from_user(w32, (const void __user *)arg, sizeof(w32)) != 0) {
        return -EFAULT;
    }
    k_memset(&w, 0, sizeof(w));
    w.buffer = w32[0];
    w.size = w32[1];
    return ia32_ioctl_native(fd, cmd, &w, sizeof(w));
}

/* ------------------------------------------------------------------------
 * KVM: its structures have one layout in both ABIs, but the two that carry
 * a pointer in a union with a 64-bit field take only its low half from a
 * compat caller, as Linux's kvm_vm_compat_ioctl does.
 */

#define KVM_GET_DIRTY_LOG   0x4010AE42u
#define KVM_CLEAR_DIRTY_LOG 0xC018AEC0u

/** KVM_GET_DIRTY_LOG and KVM_CLEAR_DIRTY_LOG with the bitmap's pointer
 * zero-extended from its low 32 bits; every other request of type 0xAE
 * passes. */
static long kvm(const fd_entry_t *e, unsigned int fd, unsigned int cmd, unsigned long arg)
{
    uint32_t log[6];
    uint32_t len = IOC_SIZE(cmd);

    (void)e;
    if (cmd != KVM_GET_DIRTY_LOG && cmd != KVM_CLEAR_DIRTY_LOG) {
        return pass(fd, cmd, arg);
    }
    if (copy_from_user(log, (const void __user *)arg, len) != 0) {
        return -EFAULT;
    }
    log[len / 4u - 1u] = 0; /* the pointer's high half, the structure's last word */
    return ia32_ioctl_native(fd, cmd, log, len);
}

/* ------------------------------------------------------------------------
 * Sockets and network devices, as Linux's compat_sock_ioctl takes them.
 */

#define SIOCADDRT        0x890Bu
#define SIOCDELRT        0x890Cu
#define SIOCGIFCONF      0x8912u
#define SIOCSIFADDR      0x8916u
#define SIOCDIFADDR      0x8936u
#define SIOCGSTAMP_OLD   0x8906u
#define SIOCGSTAMPNS_OLD 0x8907u
#define IFNAMSIZ         16u
#define IFREQ32_LEN      32u /* i386's `struct ifreq`, Linux's compat_ifreq */
#define RTENTRY32_LEN    84u
#define IN6_RTMSG32_LEN  72u

/* The native `struct ifreq`: its name and a union of 24 bytes, whose
 * `ifmap` of two longs makes it 8 bytes longer than i386's. */
struct ifreq64 {
    char name[IFNAMSIZ];
    uint8_t u[24];
};

/**
 * An interface request with i386's `struct ifreq`: its 32 bytes are the
 * native structure's first 32, as Linux's get_user_ifreq copies them for a
 * compat caller, and what the native request changes of them is copied back,
 * since a getter rewrites the structure and a setter leaves it as it was.
 * The interface requests Rach2 answers carry no pointer in the union.
 */
static long ifreq(unsigned int fd, unsigned int cmd, unsigned long arg)
{
    struct ifreq64 r;
    uint8_t was[IFREQ32_LEN];
    uint32_t i;
    long rc;

    k_memset(&r, 0, sizeof(r));
    if (copy_from_user(&r, (const void __user *)arg, IFREQ32_LEN) != 0) {
        return -EFAULT;
    }
    k_memcpy(was, &r, IFREQ32_LEN);
    rc = ia32_ioctl_native(fd, cmd, &r, sizeof(r));
    for (i = 0; rc >= 0 && i < IFREQ32_LEN; i++) {
        if (was[i] != ((const uint8_t *)&r)[i]) {
            return copy_to_user((void __user *)arg, &r, IFREQ32_LEN) != 0 ? -EFAULT : rc;
        }
    }
    return rc;
}

/**
 * SIOCGIFCONF with i386's `struct ifconf`, a length and a 32-bit pointer to
 * an array of i386's 32-byte `ifreq`: the native request fills its own array
 * of the same count here, each of whose entries' first 32 bytes, the name and
 * the address, is the i386 entry, and the length counts the i386 entries.
 * With a null pointer it gives the length every entry would take, as the
 * native request does.
 */
static long ifconf(unsigned int fd, unsigned long arg)
{
    struct ifconf32 {
        int32_t len;
        uint32_t buf;
    } c32;
    struct ifconf64 {
        int32_t len;
        int32_t pad;
        uintptr_t buf;
        struct ifreq64 req[1];
    } *c;
    uint32_t n = 0, i, size;
    long rc;

    if (copy_from_user(&c32, (const void __user *)arg, sizeof(c32)) != 0) {
        return -EFAULT;
    }
    if (c32.buf != 0 && c32.len > 0) {
        n = (uint32_t)c32.len / IFREQ32_LEN;
        if (n > (uint32_t)netdev_seen_count()) {
            n = (uint32_t)netdev_seen_count();
        }
    }
    size = (uint32_t)sizeof(*c) + n * (uint32_t)sizeof(struct ifreq64);
    c = (struct ifconf64 *)kzalloc(size);
    if (!c) {
        return -ENOMEM;
    }
    c->len = (int32_t)(n * sizeof(struct ifreq64));
    c->buf = c32.buf != 0 ? (uintptr_t)c->req : 0;
    rc = ia32_ioctl_native(fd, SIOCGIFCONF, c, size);
    if (rc == 0) {
        n = (uint32_t)c->len / (uint32_t)sizeof(struct ifreq64);
        for (i = 0; c32.buf != 0 && i < n && rc == 0; i++) {
            if (copy_to_user((void __user *)(uintptr_t)(c32.buf + i * IFREQ32_LEN), &c->req[i],
                             IFREQ32_LEN) != 0) {
                rc = -EFAULT;
            }
        }
        c32.len = (int32_t)(n * IFREQ32_LEN);
        if (rc == 0 && copy_to_user((void __user *)arg, &c32.len, sizeof(c32.len)) != 0) {
            rc = -EFAULT;
        }
    }
    kfree(c);
    return rc;
}

/**
 * SIOCADDRT and SIOCDELRT: i386's `struct rtentry`, 84 bytes, with its
 * longs and its pointer to the device's name in 32 bits, as Linux's
 * inet_compat_routing_ioctl copies it; on an AF_INET6 socket, i386's
 * `struct in6_rtmsg`, 72 bytes, whose `info` is a 32-bit long, as
 * inet6_compat_routing_ioctl copies it.
 */
static long route(const fd_entry_t *e, unsigned int fd, unsigned int cmd, unsigned long arg)
{
    uint8_t r32[RTENTRY32_LEN];
    union {
        struct {
            unsigned long pad1;
            uint8_t sockaddrs[48]; /* destination, gateway, mask */
            unsigned short flags;
            short pad2;
            unsigned long pad3;
            uintptr_t pad4;
            short metric;
            uintptr_t dev;
            unsigned long mtu;
            unsigned long window;
            unsigned short irtt;
        } rt;
        struct {
            uint8_t addrs[48]; /* destination, source, gateway */
            uint32_t type;
            uint16_t dst_len;
            uint16_t src_len;
            uint32_t metric;
            unsigned long info;
            uint32_t flags;
            int32_t ifindex;
        } in6;
    } r;
    int inet6 = e->type == FD_TYPE_SOCKET && e->sock && e->sock->family == AF_INET6;
    uint32_t w;

    if (copy_from_user(r32, (const void __user *)arg, inet6 ? IN6_RTMSG32_LEN : RTENTRY32_LEN) !=
        0) {
        return -EFAULT;
    }
    k_memset(&r, 0, sizeof(r));
    if (inet6) {
        k_memcpy(r.in6.addrs, r32, 48);
        k_memcpy(&r.in6.type, r32 + 48, 4);
        k_memcpy(&r.in6.dst_len, r32 + 52, 2);
        k_memcpy(&r.in6.src_len, r32 + 54, 2);
        k_memcpy(&r.in6.metric, r32 + 56, 4);
        k_memcpy(&w, r32 + 60, 4);
        r.in6.info = w;
        k_memcpy(&r.in6.flags, r32 + 64, 4);
        k_memcpy(&r.in6.ifindex, r32 + 68, 4);
        return ia32_ioctl_native(fd, cmd, &r.in6, sizeof(r.in6));
    }
    k_memcpy(r.rt.sockaddrs, r32 + 4, 48);
    k_memcpy(&r.rt.flags, r32 + 52, 2);
    k_memcpy(&r.rt.metric, r32 + 64, 2);
    k_memcpy(&w, r32 + 68, 4);
    r.rt.dev = w;
    k_memcpy(&w, r32 + 72, 4);
    r.rt.mtu = w;
    k_memcpy(&w, r32 + 76, 4);
    r.rt.window = w;
    k_memcpy(&r.rt.irtt, r32 + 80, 2);
    return ia32_ioctl_native(fd, cmd, &r.rt, sizeof(r.rt));
}

/**
 * The requests of type 0x89, by Linux's compat_sock_ioctl_trans: the
 * interface requests with i386's `ifreq`, SIOCGIFCONF and the routes
 * translated; the ones of an `int`, an `arpreq`, a bridge's or a VLAN's
 * argument, SIOCGSKNS and SIOCGSTAMP and SIOCGSTAMPNS pass, as do the
 * AF_INET6 address requests, whose `in6_ifreq` has one layout; the old
 * SIOCGSTAMP and SIOCGSTAMPNS write i386's `old_timeval32` and
 * `old_timespec32` themselves while `in_compat_syscall` is true
 * (`socket_gettstamp`); the rest, the requests that carry an `ifr_data`
 * pointer and the private ones among them, which Linux translates and Rach2
 * does not answer, are -ENOTTY. A descriptor that is no socket passes, for
 * the native path's answer.
 */
static long sock(const fd_entry_t *e, unsigned int fd, unsigned int cmd, unsigned long arg)
{
    if (e->type != FD_TYPE_SOCKET && e->type != FD_TYPE_UNIX) {
        return pass(fd, cmd, arg);
    }
    switch (cmd) {
    case SIOCGIFCONF:
        return ifconf(fd, arg);
    case SIOCADDRT:
    case SIOCDELRT:
        return route(e, fd, cmd, arg);
    case SIOCSIFADDR:
    case SIOCDIFADDR:
        if (e->type == FD_TYPE_SOCKET && e->sock && e->sock->family == AF_INET6) {
            return pass(fd, cmd, arg);
        }
        return ifreq(fd, cmd, arg);
    case 0x8901u: /* FIOSETOWN */
    case 0x8902u: /* SIOCSPGRP */
    case 0x8903u: /* FIOGETOWN */
    case 0x8904u: /* SIOCGPGRP */
    case 0x8905u: /* SIOCATMARK */
    case SIOCGSTAMP_OLD:
    case SIOCGSTAMPNS_OLD:
    case 0x80108906u: /* SIOCGSTAMP_NEW */
    case 0x80108907u: /* SIOCGSTAMPNS_NEW */
    case 0x8940u:     /* SIOCGIFBR */
    case 0x8941u:     /* SIOCSIFBR */
    case 0x894Bu:     /* SIOCOUTQNSD */
    case 0x894Cu:     /* SIOCGSKNS */
    case 0x8953u:     /* SIOCDARP */
    case 0x8954u:     /* SIOCGARP */
    case 0x8955u:     /* SIOCSARP */
    case 0x8982u:     /* SIOCGIFVLAN */
    case 0x8983u:     /* SIOCSIFVLAN */
    case 0x89A0u:     /* SIOCBRADDBR */
    case 0x89A1u:     /* SIOCBRDELBR */
        return pass(fd, cmd, arg);
    case 0x8910u: /* SIOCGIFNAME */
    case 0x8913u: /* SIOCGIFFLAGS */
    case 0x8914u: /* SIOCSIFFLAGS */
    case 0x8915u: /* SIOCGIFADDR */
    case 0x8917u: /* SIOCGIFDSTADDR */
    case 0x8918u: /* SIOCSIFDSTADDR */
    case 0x8919u: /* SIOCGIFBRDADDR */
    case 0x891Au: /* SIOCSIFBRDADDR */
    case 0x891Bu: /* SIOCGIFNETMASK */
    case 0x891Cu: /* SIOCSIFNETMASK */
    case 0x891Du: /* SIOCGIFMETRIC */
    case 0x891Eu: /* SIOCSIFMETRIC */
    case 0x891Fu: /* SIOCGIFMEM */
    case 0x8920u: /* SIOCSIFMEM */
    case 0x8921u: /* SIOCGIFMTU */
    case 0x8922u: /* SIOCSIFMTU */
    case 0x8923u: /* SIOCSIFNAME */
    case 0x8924u: /* SIOCSIFHWADDR */
    case 0x8927u: /* SIOCGIFHWADDR */
    case 0x8931u: /* SIOCADDMULTI */
    case 0x8932u: /* SIOCDELMULTI */
    case 0x8933u: /* SIOCGIFINDEX */
    case 0x8934u: /* SIOCSIFPFLAGS */
    case 0x8935u: /* SIOCGIFPFLAGS */
    case 0x8937u: /* SIOCSIFHWBROADCAST */
    case 0x8942u: /* SIOCGIFTXQLEN */
    case 0x8943u: /* SIOCSIFTXQLEN */
    case 0x8947u: /* SIOCGMIIPHY */
    case 0x8948u: /* SIOCGMIIREG */
    case 0x8949u: /* SIOCSMIIREG */
    case 0x8970u: /* SIOCGIFMAP */
    case 0x8971u: /* SIOCSIFMAP */
    case 0x8990u: /* SIOCBONDENSLAVE */
    case 0x8991u: /* SIOCBONDRELEASE */
    case 0x8992u: /* SIOCBONDSETHWADDR */
    case 0x8995u: /* SIOCBONDCHANGEACTIVE */
    case 0x89A2u: /* SIOCBRADDIF */
    case 0x89A3u: /* SIOCBRDELIF */
        return ifreq(fd, cmd, arg);
    default:
        return -ENOTTY;
    }
}

/** The wireless extensions' requests, of type 0x8B: an `iwreq` is 32 bytes
 * in both ABIs, with its name where an `ifreq` has it, so the requests go as
 * interface requests. Rach2 answers SIOCGIWNAME alone, whose answer is in
 * the union; Linux translates the ones that carry an `iw_point`. */
static long wext(const fd_entry_t *e, unsigned int fd, unsigned int cmd, unsigned long arg)
{
    if (e->type != FD_TYPE_SOCKET && e->type != FD_TYPE_UNIX) {
        return pass(fd, cmd, arg);
    }
    return ifreq(fd, cmd, arg);
}

/* ------------------------------------------------------------------------
 * The families, by the type of the request's number.
 */

#define TIOCGSERIAL 0x541Eu
#define TIOCSSERIAL 0x541Fu
#define GIO_UNIMAP  0x4B66u
#define PIO_UNIMAP  0x4B67u
#define KDFONTOP    0x4B72u

/** The terminal and console requests of types 'T' and 'K': their arguments
 * have one layout in both ABIs, `termios`, `termios2`, `winsize`, an `int`
 * or the keyboard's tables, and they pass; the five Linux translates, whose
 * structures differ and which Rach2 does not answer, are -ENOTTY, so that
 * one added natively is not passed untranslated. Type 'T' also holds the
 * requests every descriptor answers, FIONBIO, FIONREAD, FIOCLEX and the
 * like. */
static long tty(const fd_entry_t *e, unsigned int fd, unsigned int cmd, unsigned long arg)
{
    (void)e;
    if (cmd == TIOCGSERIAL || cmd == TIOCSSERIAL || cmd == GIO_UNIMAP || cmd == PIO_UNIMAP ||
        cmd == KDFONTOP) {
        return -ENOTTY;
    }
    return pass(fd, cmd, arg);
}

/** The DRM requests, type 'd' (ia32_ioctl_drm.c). */
static long drm(const fd_entry_t *e, unsigned int fd, unsigned int cmd, unsigned long arg)
{
    return drm_is_file(e) ? ia32_ioctl_drm(fd, cmd, arg) : pass(fd, cmd, arg);
}

/** The requests of dma-buf, sync_file and udmabuf (ia32_ioctl_gpu.c). */
static long gpu(const fd_entry_t *e, unsigned int fd, unsigned int cmd, unsigned long arg)
{
    (void)e;
    return ia32_ioctl_gpu(fd, cmd, arg);
}

/* A family: the type of its requests' numbers, and the handler of a compat
 * caller's request, or NULL where every request passes. */
struct family {
    uint8_t type;
    long (*fn)(const fd_entry_t *e, unsigned int fd, unsigned int cmd, unsigned long arg);
};

static const struct family families[] = {
    {0x03, hdio},    /* HDIO_GETGEO */
    {0x12, blk},     /* block devices */
    {0x3B, NULL},    /* ';', VFIO */
    {0x3E, gpu},     /* sync_file */
    {0x41, audio},   /* the console's AUDIO_* */
    {0x45, evdev},   /* 'E', evdev */
    {0x46, NULL},    /* 'F', the frame buffer's FB_* of <rach2/fb.h> */
    {0x4B, tty},     /* 'K', the console's keyboard */
    {0x4C, loop},    /* 'L', loop devices */
    {0x4D, NULL},    /* 'M', the OSS mixer */
    {0x50, NULL},    /* 'P', the OSS dsp */
    {0x52, NULL},    /* 'R', rfkill */
    {0x54, tty},     /* 'T', terminals and the requests of every descriptor */
    {0x56, NULL},    /* 'V', virtual terminals */
    {0x57, NULL},    /* 'W', watchdog and sw_sync */
    {0x62, gpu},     /* 'b', dma-buf */
    {0x64, drm},     /* 'd', DRM */
    {0x66, fs_attr}, /* 'f', file attributes */
    {0x70, NULL},    /* 'p', the RTC */
    {0x75, gpu},     /* 'u', udmabuf */
    {0x76, fs_attr}, /* 'v', a file's generation */
    {0x77, NULL},    /* 'w', wsim */
    {0x89, sock},    /* sockets and network devices */
    {0x8B, wext},    /* wireless extensions */
    {0x94, NULL},    /* FICLONE and FICLONERANGE */
    {0xAE, kvm},     /* KVM */
    {0xB7, NULL},    /* namespaces */
};

#define FAMILIES ((int)(sizeof(families) / sizeof(families[0])))

/**
 * ioctl(2) from an i386 caller.
 *
 * The descriptor is checked first, -EBADF for a bad or O_PATH one, as the
 * native call checks it. The request then goes to its family's handler by
 * the type of its number, with its pointer zero-extended: a family whose
 * arguments have one layout in both ABIs passes it to the native path, and
 * one whose arguments differ translates the requests that need it, as
 * Linux's compat handler of the family does. A request of a type no family
 * holds is -ENOTTY, as Linux answers a compat caller on a file with no
 * compat handler; rach2/ia32.md lists the families.
 */
long compat_sys_ioctl(isr_params_t *params)
{
    unsigned int fd = (unsigned int)SC_ARG(params, 0);
    unsigned int cmd = (unsigned int)SC_ARG(params, 1);
    unsigned long arg = SC_ARG(params, 2);
    fd_entry_t *e;
    int i;

    if (!current_proc) {
        return -ESRCH;
    }
    e = fd_get(current_proc->fds, (int)fd);
    if (!e || (e->flags & FD_FLAG_PATH)) {
        return -EBADF;
    }
    for (i = 0; i < FAMILIES; i++) {
        if (families[i].type == IOC_TYPE(cmd)) {
            return families[i].fn ? families[i].fn(e, fd, cmd, arg) : pass(fd, cmd, arg);
        }
    }
    return -ENOTTY;
}

#endif /* RACH2_IA32 */
