/**
 * ia32_ioctl_hosttest.c: the compat ioctl path of the x86-64 kernel,
 * rach2/sys/arch/x86_64/ia32_ioctl.c and ia32_ioctl_drm.c, against Linux
 * 6.12's i386 layouts.
 *
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-deps: tests/host/compat/kheap_host.c
 * host-cflags: -DRACH2_HOST_X86_64
 *
 * The three files of the path are included whole. A row makes an i386
 * ioctl: a frame of `int $0x80` with the descriptor, the number and a user
 * address in EBX, ECX and EDX. The caller's memory is an arena, whose
 * offsets are the user addresses, so that a 32-bit pointer reaches it on
 * any host; one below 4096 faults. The native path is a model of
 * `ioctl_fd`, which notes the number and the argument it was given and, for
 * a translated request, the structure in the kernel buffer the user-copy
 * window covers, and answers what the row asks of it. The i386 structures
 * are built byte by byte at Linux's offsets, which were read from Linux
 * 6.12's headers compiled with `gcc -m32`, so a row checks the layout the
 * path reads and writes and not the one it declares.
 *
 * The rows: the descriptor's check, a type no family holds and the
 * requests refused; the families that pass, with the pointer as it was;
 * chattr's numbers; BLKGETSIZE, BLKRAGET and HDIO_GETGEO; LOOP_SET_STATUS
 * and LOOP_GET_STATUS with `loop_info`; the bit sets of evdev; AUDIO_WRITE;
 * KVM's dirty log; the interface requests with the 32-byte `ifreq`,
 * SIOCGIFCONF, and the routes of AF_INET and AF_INET6; and the DRM core's
 * VERSION, GET_UNIQUE, GET_CLIENT, GET_STATS, SET_UNIQUE, UPDATE_DRAW,
 * WAIT_VBLANK, ADDFB2 and GETFB2, a driver's request and a request that
 * passes.
 *
 * Returns 0 when every row passes, 1 otherwise.
 */

#include "arch/x86_64/ia32_ioctl.c"
#include "arch/x86_64/ia32_ioctl_drm.c"
#include "arch/x86_64/ia32_ioctl_gpu.c"

extern int printf(const char *format, ...);
extern void *memcpy(void *dst, const void *src, unsigned long n);
extern void *memset(void *dst, int c, unsigned long n);
extern int memcmp(const void *a, const void *b, unsigned long n);

static int fails;

#define CHECK(what, cond)                                \
    do {                                                 \
        if (!(cond)) {                                   \
            printf("ia32-ioctl-test: fail: %s\n", what); \
            fails++;                                     \
        }                                                \
    } while (0)

struct percpu cpu_table[NR_CPUS];

/* ---- the caller's memory ---- */

#define ARENA 0x10000u
static uint8_t arena[ARENA];

/** The host address of the user address `u`. */
static uint8_t *U(uint32_t u)
{
    return arena + u;
}

/* The window of kernel memory the path opened, and the copies made. */
static uint8_t *win_base;
static uint32_t win_len;
static int to_user_copies;

void user_window_open(void *buf, uint32_t len)
{
    CHECK("one window at a time", win_base == NULL);
    win_base = (uint8_t *)buf;
    win_len = len;
}

void user_window_close(void)
{
    win_base = NULL;
    win_len = 0;
}

/** The host address of the `n` bytes at the address `a`, which is in the
 * window or the arena, or NULL for a fault. */
static uint8_t *reach(uintptr_t a, size_t n)
{
    if (win_base && (uint8_t *)a >= win_base && (uint8_t *)a + n <= win_base + win_len) {
        return (uint8_t *)a;
    }
    if (a >= 4096 && a + n <= ARENA) {
        return arena + a;
    }
    return NULL;
}

int copy_to_user(void __user *dst, const void *src, size_t n)
{
    uint8_t *p = reach((uintptr_t)dst, n);

    if (!p) {
        return -1;
    }
    if (p < win_base || p >= win_base + win_len) {
        to_user_copies++;
    }
    memcpy(p, src, n);
    return 0;
}

int copy_from_user(void *dst, const void __user *src, size_t n)
{
    const uint8_t *p = reach((uintptr_t)src, n);

    if (!p) {
        return -1;
    }
    memcpy(dst, p, n);
    return 0;
}

/* ---- descriptors ---- */

static fd_entry_t entries[8];
static fd_table_t table = {entries, 8, 1, 0, NULL};
static process_t proc;
static struct socket inet6_sock;

fd_entry_t *fd_get(fd_table_t *t, int fd)
{
    if (!t || fd < 0 || fd >= t->cap || t->entries[fd].type == FD_TYPE_NONE) {
        return NULL;
    }
    return &t->entries[fd];
}

/* The descriptors: a device, a socket of AF_INET, one of AF_INET6, the
 * console and one opened with O_PATH. */
enum { FD_DEV = 3, FD_INET = 4, FD_INET6 = 5, FD_CON = 6, FD_PATHFD = 7 };

/** Three interfaces with an IPv4 address. */
int netdev_seen_count(void)
{
    return 3;
}

/* ---- the native path ---- */

static int native_calls;
static unsigned int native_cmd;
static uintptr_t native_arg;
static int native_in_window;
static uint32_t native_len; /* the window's length during the call */
/* A native request of the row's own, which acts on the window itself. */
static void (*native_hook)(uint8_t *buf);
static uint8_t native_saw[512];   /* the kernel buffer as the native path got it */
static uint8_t native_reply[512]; /* what it writes there, `reply_len` bytes */
static uint32_t reply_len;
static long native_rc;

long ioctl_fd(int fd, uint32_t request, uintptr_t arg)
{
    (void)fd;
    native_calls++;
    native_cmd = request;
    native_arg = arg;
    native_in_window = win_base != NULL && (uint8_t *)arg == win_base;
    native_len = win_len;
    if (native_in_window) {
        memcpy(native_saw, win_base, win_len < sizeof(native_saw) ? win_len : sizeof(native_saw));
        if (native_hook) {
            native_hook(win_base);
        } else if (reply_len) {
            CHECK("the reply fits the window", reply_len <= win_len);
            memcpy(win_base, native_reply, reply_len);
        }
    }
    return native_rc;
}

/* ---- the frame ---- */

/** Reset the model and the arena for a row. */
static void fresh(void)
{
    native_calls = 0;
    native_cmd = 0;
    native_arg = 0;
    native_in_window = 0;
    native_len = 0;
    native_hook = NULL;
    memset(native_saw, 0, sizeof(native_saw));
    memset(native_reply, 0, sizeof(native_reply));
    reply_len = 0;
    native_rc = 0;
    to_user_copies = 0;
    memset(arena, 0xEE, sizeof(arena));
}

/** The i386 ioctl `cmd` on `fd` with the user address `arg`. */
static long ioctl32(int fd, uint32_t cmd, uint32_t arg)
{
    isr_params_t p;

    memset(&p, 0, sizeof(p));
    p.vector = 0x80;
    p.error_code = SC_ENTRY_IA32;
    p.rax = 54;
    p.rbx = 0xFFFFFFFF00000000ull | (uint32_t)fd; /* the upper halves are not the call's */
    p.rcx = 0xFFFFFFFF00000000ull | cmd;
    p.rdx = 0xFFFFFFFF00000000ull | arg;
    return compat_sys_ioctl(&p);
}

/** Store a 32-bit value at user address U. */
static void put32(uint32_t u, uint32_t v)
{
    memcpy(U(u), &v, 4);
}

/** The 32-bit value at user address U. */
static uint32_t get32(uint32_t u)
{
    uint32_t v;

    memcpy(&v, U(u), 4);
    return v;
}

/** The 32-bit value at OFF of what the native call was handed. */
static uint32_t saw32(uint32_t off)
{
    uint32_t v;

    memcpy(&v, native_saw + off, 4);
    return v;
}

/** The 64-bit value at OFF of what the native call was handed. */
static uint64_t saw64(uint32_t off)
{
    uint64_t v;

    memcpy(&v, native_saw + off, 8);
    return v;
}

/** Set the 32-bit value at OFF of the native call's reply. */
static void reply32(uint32_t off, uint32_t v)
{
    memcpy(native_reply + off, &v, 4);
}

/** Set the 64-bit value at OFF of the native call's reply. */
static void reply64(uint32_t off, uint64_t v)
{
    memcpy(native_reply + off, &v, 8);
}

/* ---- the rows ---- */

/** The descriptor first, then the family by the number's type. */
static void test_path(void)
{
    fresh();
    CHECK("a closed descriptor is EBADF", ioctl32(2, 0x5401, 0x1000) == -EBADF);
    CHECK("an O_PATH descriptor is EBADF", ioctl32(FD_PATHFD, 0x5401, 0x1000) == -EBADF);
    CHECK("an out-of-range descriptor is EBADF", ioctl32(100, 0x5401, 0x1000) == -EBADF);
    CHECK("no native call for a bad descriptor", native_calls == 0);

    fresh();
    CHECK("FIFREEZE, of no family, is ENOTTY", ioctl32(FD_DEV, 0xC0045877u, 0x1000) == -ENOTTY);
    CHECK("TIOCGSERIAL is ENOTTY", ioctl32(FD_DEV, 0x541E, 0x1000) == -ENOTTY);
    CHECK("TIOCSSERIAL is ENOTTY", ioctl32(FD_DEV, 0x541F, 0x1000) == -ENOTTY);
    CHECK("KDFONTOP is ENOTTY", ioctl32(FD_DEV, 0x4B72, 0x1000) == -ENOTTY);
    CHECK("GIO_UNIMAP is ENOTTY", ioctl32(FD_DEV, 0x4B66, 0x1000) == -ENOTTY);
    CHECK("PIO_UNIMAP is ENOTTY", ioctl32(FD_DEV, 0x4B67, 0x1000) == -ENOTTY);
    CHECK("the old SIOCGSTAMP is ENOTTY", ioctl32(FD_INET, 0x8906, 0x1000) == -ENOTTY);
    CHECK("SIOCETHTOOL is ENOTTY", ioctl32(FD_INET, 0x8946, 0x1000) == -ENOTTY);
    CHECK("SIOCDEVPRIVATE is ENOTTY", ioctl32(FD_INET, 0x89F0, 0x1000) == -ENOTTY);
    CHECK("no native call for a refused request", native_calls == 0);
}

/** The families whose arguments have one layout pass the number and the
 * pointer as they were, and the native result. */
static void test_pass(void)
{
    static const uint32_t cmds[] = {
        0x5401,     /* TCGETS */
        0x802C542A, /* TCGETS2 */
        0x5413,     /* TIOCGWINSZ */
        0x541B,     /* FIONREAD */
        0x5421,     /* FIONBIO */
        0x4B44,     /* KDGKBMODE */
        0x4B4A,     /* KDGKBDIACR */
        0x80044D00, /* SOUND_MIXER_READ_VOLUME */
        0xC0045002, /* SNDCTL_DSP_SPEED */
        0x8010500D, /* SNDCTL_DSP_GETOSPACE */
        0x4601,     /* FB_MODE_13H, which is FBIOPUT_VSCREENINFO's number */
        0x4602,     /* FB_MODE_TEXT, which is FBIOGET_FSCREENINFO's */
        0x4C04,     /* LOOP_SET_STATUS64 */
        0x4C0A,     /* LOOP_CONFIGURE */
        0x80041272, /* BLKGETSIZE64, numbered with i386's size_t */
        0x80041270, /* BLKBSZGET, likewise */
        0x1268,     /* BLKSSZGET */
        0x80247009, /* RTC_RD_TIME */
        0x40045201, /* rfkill's RFKILL_IOCTL_NOINPUT, any 'R' */
        0x80285700, /* WDIOC_GETSUPPORT */
        0x80107701, /* WSIM_IOC_INFO */
        0x40049409, /* FICLONE */
        0xB701,     /* NS_GET_USERNS */
        0x80044501, /* EVIOCGVERSION */
        0x80404506, /* EVIOCGNAME(64) */
        0x80184540, /* EVIOCGABS(ABS_X) */
        0xAE80,     /* KVM_RUN */
        0x4004AE8B, /* KVM_SET_SIGNAL_MASK */
        0x4104,     /* AUDIO_WRITE off the console */
        0x4101,     /* AUDIO_GET_INFO */
    };
    int i;

    for (i = 0; i < (int)(sizeof(cmds) / sizeof(cmds[0])); i++) {
        fresh();
        native_rc = 7;
        CHECK("a passed request returns the native result", ioctl32(FD_DEV, cmds[i], 0x1234) == 7);
        CHECK("a passed request keeps its number", native_cmd == cmds[i]);
        CHECK("a passed request keeps its pointer", native_arg == 0x1234);
        CHECK("a passed request opens no window", !native_in_window && native_calls == 1);
    }
    fresh();
    CHECK("an AF_INET6 address request passes", ioctl32(FD_INET6, 0x8916, 0x1200) == 0);
    CHECK("its in6_ifreq is the caller's", native_arg == 0x1200 && native_cmd == 0x8916);
    fresh();
    CHECK("SIOCSARP passes", ioctl32(FD_INET, 0x8955, 0x1200) == 0 && native_arg == 0x1200);
    fresh();
    CHECK("SIOCGIFFLAGS on a device passes", ioctl32(FD_DEV, 0x8913, 0x1200) == 0);
    CHECK("there its pointer is the caller's", native_arg == 0x1200);
}

/** chattr's numbers with i386's 4-byte long become the native ones. */
static void test_fs_attr(void)
{
    static const uint32_t in[] = {0x80046601, 0x40046602, 0x80047601, 0x40047602, 0x8020660B};
    static const uint32_t out[] = {0x80086601, 0x40086602, 0x80087601, 0x40087602, 0x8020660B};
    int i;

    for (i = 0; i < 5; i++) {
        fresh();
        CHECK("a file attribute request reaches the native path",
              ioctl32(FD_DEV, in[i], 0x1100) == 0 && native_arg == 0x1100);
        CHECK("a file attribute request has the native number", native_cmd == out[i]);
    }
}

/** BLKGETSIZE, BLKRAGET and HDIO_GETGEO, in i386's sizes. */
static void test_block(void)
{
    fresh();
    reply64(0, 0x12345678u);
    reply_len = 8;
    CHECK("BLKGETSIZE answers", ioctl32(FD_DEV, 0x1260, 0x1000) == 0);
    CHECK("BLKGETSIZE runs natively on 8 bytes", native_in_window && native_cmd == 0x1260);
    CHECK("BLKGETSIZE gives 4 bytes", get32(0x1000) == 0x12345678u && U(0x1004)[0] == 0xEE);

    fresh();
    reply64(0, 0x100000000ull);
    reply_len = 8;
    CHECK("BLKGETSIZE past 32 bits is EFBIG", ioctl32(FD_DEV, 0x1260, 0x1000) == -EFBIG);
    CHECK("an EFBIG writes nothing", U(0x1000)[0] == 0xEE);

    fresh();
    reply64(0, 256);
    reply_len = 8;
    CHECK("BLKRAGET answers", ioctl32(FD_DEV, 0x1263, 0x1000) == 0 && get32(0x1000) == 256);
    CHECK("BLKRAGET gives 4 bytes", U(0x1004)[0] == 0xEE);
    fresh();
    CHECK("BLKRAGET into a null pointer is EINVAL", ioctl32(FD_DEV, 0x1263, 0) == -EINVAL);
    fresh();
    native_rc = -ENOTTY;
    CHECK("BLKRAGET of another device is its answer", ioctl32(FD_DEV, 0x1263, 0) == -ENOTTY);

    fresh();
    native_reply[0] = 255;
    native_reply[1] = 63;
    native_reply[2] = 0xE8;
    native_reply[3] = 0x03;
    reply64(8, 0x100000802ull);
    reply_len = 16;
    CHECK("HDIO_GETGEO answers", ioctl32(FD_DEV, 0x0301, 0x1000) == 0);
    CHECK("HDIO_GETGEO's native buffer is 16 bytes", native_in_window && native_len == 16);
    CHECK("hd_geometry's heads, sectors and cylinders", U(0x1000)[0] == 255 && U(0x1000)[1] == 63 &&
                                                            U(0x1000)[2] == 0xE8 &&
                                                            U(0x1000)[3] == 0x03);
    CHECK("hd_geometry's start is at 4, cut to 32 bits", get32(0x1004) == 0x802);
    CHECK("hd_geometry is 8 bytes", U(0x1008)[0] == 0xEE);
    fresh();
    CHECK("HDIO_GETGEO into a null pointer is EINVAL", ioctl32(FD_DEV, 0x0301, 0) == -EINVAL);
    fresh();
    CHECK("another HDIO request is ENOTTY", ioctl32(FD_DEV, 0x030C, 0x1000) == -ENOTTY);
    CHECK("and runs nothing", native_calls == 0);
}

/* i386's `struct loop_info`: number 0, device 4, inode 8, rdevice 12, offset
 * 16, encrypt_type 20, encrypt_key_size 24, flags 28, name 32, key 96, init
 * 128, reserved 136; 140 bytes. `loop_info64`: device 0, inode 8, rdevice
 * 16, offset 24, sizelimit 32, number 40, encrypt_type 44, key size 48,
 * flags 52, file_name 56, crypt_name 120, key 184, init 216; 232 bytes. */
static void test_loop(void)
{
    CHECK("loop_info32 is 140 bytes", sizeof(struct loop_info32) == 140);
    CHECK("loop_info64 is 232 bytes", sizeof(struct loop_info64) == 232);

    fresh();
    memset(U(0x1000), 0, 140);
    put32(0x1000, 3);
    U(0x1004)[0] = 0x07;
    U(0x1004)[1] = 0x01;
    put32(0x1008, 4242);
    put32(0x1010, 0);
    put32(0x1014, 9); /* encrypt_type, which Linux does not carry */
    put32(0x101C, 4);
    memcpy(U(0x1020), "/img.ext2", 10);
    CHECK("LOOP_SET_STATUS answers", ioctl32(FD_DEV, 0x4C02, 0x1000) == 0);
    CHECK("LOOP_SET_STATUS runs LOOP_SET_STATUS64", native_cmd == 0x4C04 && native_in_window);
    CHECK("loop_info64's device", saw64(0) == 0x107);
    CHECK("loop_info64's inode", saw64(8) == 4242);
    CHECK("loop_info64's offset and size limit", saw64(24) == 0 && saw64(32) == 0);
    CHECK("loop_info64's number and flags", saw32(40) == 3 && saw32(52) == 4);
    CHECK("loop_info64's encryption is left out", saw32(44) == 0);
    CHECK("loop_info64's name", memcmp(native_saw + 56, "/img.ext2", 10) == 0);
    fresh();
    CHECK("LOOP_SET_STATUS from a bad pointer is EFAULT",
          ioctl32(FD_DEV, 0x4C02, 0x10) == -EFAULT && native_calls == 0);

    fresh();
    reply64(0, 0x0702);
    reply64(8, 77);
    reply64(16, 0);
    reply32(40, 1);
    reply32(52, 5);
    memcpy(native_reply + 56, "/root.img", 10);
    reply_len = 232;
    CHECK("LOOP_GET_STATUS answers", ioctl32(FD_DEV, 0x4C03, 0x1000) == 0);
    CHECK("LOOP_GET_STATUS runs LOOP_GET_STATUS64", native_cmd == 0x4C05);
    CHECK("loop_info's number", get32(0x1000) == 1);
    CHECK("loop_info's device", U(0x1004)[0] == 0x02 && U(0x1004)[1] == 0x07);
    CHECK("loop_info's inode", get32(0x1008) == 77);
    CHECK("loop_info's flags", get32(0x101C) == 5);
    CHECK("loop_info's name", memcmp(U(0x1020), "/root.img", 10) == 0);
    CHECK("loop_info is 140 bytes", U(0x1000 + 139)[0] == 0 && U(0x1000 + 140)[0] == 0xEE);

    fresh();
    reply64(8, 0x100000000ull);
    reply_len = 232;
    CHECK("an inode past 32 bits is EOVERFLOW", ioctl32(FD_DEV, 0x4C03, 0x1000) == -EOVERFLOW);
    CHECK("an EOVERFLOW writes nothing", U(0x1000)[0] == 0xEE);
    fresh();
    reply64(0, 0x10000);
    reply_len = 232;
    CHECK("a device past 16 bits is EOVERFLOW", ioctl32(FD_DEV, 0x4C03, 0x1000) == -EOVERFLOW);
    fresh();
    CHECK("LOOP_GET_STATUS into a null pointer is EINVAL",
          ioctl32(FD_DEV, 0x4C03, 0) == -EINVAL && native_calls == 0);
    fresh();
    native_rc = -ENXIO;
    CHECK("LOOP_GET_STATUS of a free device is its answer",
          ioctl32(FD_DEV, 0x4C03, 0x1000) == -ENXIO && U(0x1000)[0] == 0xEE);
}

/** A bit set read of evdev by the request `cmd`, the native path giving
 * `native` bytes: the result, which the caller got that many bytes of. */
static long evbits(uint32_t cmd, long native)
{
    long rc;
    int i;

    fresh();
    for (i = 0; i < 128; i++) {
        native_reply[i] = (uint8_t)(0xA0 + i);
    }
    reply_len = (uint32_t)native;
    native_rc = native;
    rc = ioctl32(FD_DEV, cmd, 0x1000);
    if (rc > 0) {
        CHECK("the bits come back as read", memcmp(U(0x1000), native_reply, (size_t)rc) == 0);
        CHECK("no bit past the result", U(0x1000 + (uint32_t)rc)[0] == 0xEE);
    }
    return rc;
}

/** The evdev bit sets, cut to the compat length of their bits. */
static void test_evdev(void)
{
    CHECK("EVIOCGBIT(0, 64) gives 4 bytes", evbits(0x80404520, 8) == 4);
    CHECK("its native read is the caller's length", native_cmd == 0x80404520 && native_len == 64);
    CHECK("EVIOCGBIT(EV_KEY, 96) gives 96", evbits(0x80604521, 96) == 96);
    CHECK("EVIOCGBIT(EV_KEY, 4096) asks 128 natively",
          evbits(0x90004521, 96) == 96 && native_cmd == 0x80804521 && native_len == 128);
    CHECK("EVIOCGBIT(EV_REL) gives 4", evbits(0x80084522, 8) == 4);
    CHECK("EVIOCGBIT(EV_ABS) gives 8", evbits(0x80084523, 8) == 8);
    CHECK("EVIOCGBIT(EV_MSC) gives 4", evbits(0x80084524, 8) == 4);
    CHECK("EVIOCGBIT(EV_SW) gives 4", evbits(0x80084525, 8) == 4);
    CHECK("EVIOCGBIT(EV_LED) gives 4", evbits(0x80084531, 8) == 4);
    CHECK("EVIOCGBIT(EV_SND) gives 4", evbits(0x80084532, 8) == 4);
    CHECK("EVIOCGBIT(EV_FF) gives 16", evbits(0x80104535, 16) == 16);
    CHECK("EVIOCGBIT with a short buffer gives what fits", evbits(0x80024520, 2) == 2);
    CHECK("EVIOCGKEY gives 96", evbits(0x80604518, 96) == 96);
    CHECK("EVIOCGLED gives 4", evbits(0x80084519, 8) == 4);
    CHECK("EVIOCGSND gives 4", evbits(0x8008451A, 8) == 4);
    CHECK("EVIOCGSW gives 4", evbits(0x8008451B, 8) == 4);
    CHECK("EVIOCGPROP gives 4", evbits(0x80084509, 8) == 4);
    CHECK("an error of a bit set read is its own", evbits(0x80084550, -EINVAL) == -EINVAL);
    fresh();
    native_rc = 8;
    reply_len = 8;
    CHECK("a bit set read into a bad pointer is EFAULT",
          ioctl32(FD_DEV, 0x80084519, 0x10) == -EFAULT);
}

/** AUDIO_WRITE on the console: its i386 argument is a pointer and a length
 * of 4 bytes each. */
static void test_audio(void)
{
    fresh();
    put32(0x1000, 0x2000);
    put32(0x1004, 512);
    native_rc = 512;
    CHECK("AUDIO_WRITE answers", ioctl32(FD_CON, 0x4104, 0x1000) == 512);
    CHECK("AUDIO_WRITE runs natively", native_cmd == 0x4104 && native_in_window);
    CHECK("its samples' pointer is zero-extended", saw64(0) == 0x2000);
    CHECK("its length follows at 8", saw32(8) == 512);
    fresh();
    CHECK("AUDIO_WRITE from a null pointer is EFAULT", ioctl32(FD_CON, 0x4104, 0) == -EFAULT);
}

/** KVM's dirty log takes the bitmap's pointer from its low half alone. */
static void test_kvm(void)
{
    fresh();
    put32(0x1000, 2);
    put32(0x1004, 0);
    put32(0x1008, 0x3000);
    put32(0x100C, 0xDEADBEEF);
    CHECK("KVM_GET_DIRTY_LOG answers", ioctl32(FD_DEV, 0x4010AE42, 0x1000) == 0);
    CHECK("KVM_GET_DIRTY_LOG runs natively", native_cmd == 0x4010AE42 && native_in_window);
    CHECK("its slot", saw32(0) == 2);
    CHECK("its pointer zero-extended", saw64(8) == 0x3000);

    fresh();
    put32(0x1000, 1);
    put32(0x1004, 64);
    put32(0x1008, 128);
    put32(0x100C, 0);
    put32(0x1010, 0x3000);
    put32(0x1014, 0xDEADBEEF);
    CHECK("KVM_CLEAR_DIRTY_LOG answers", ioctl32(FD_DEV, 0xC018AEC0, 0x1000) == 0);
    CHECK("its pages", saw32(4) == 64 && saw64(8) == 128);
    CHECK("its pointer zero-extended", saw64(16) == 0x3000);
}

/** The interface requests with the 32-byte ifreq, SIOCGIFCONF, and the
 * routes. */
static void test_net(void)
{
    CHECK("the native ifreq is 40 bytes", sizeof(struct ifreq64) == 40);

    fresh();
    memset(U(0x1000), 0, 32);
    memcpy(U(0x1000), "eth0", 5);
    memcpy(native_reply, "eth0", 5);
    native_reply[16] = 0x43; /* IFF_UP | IFF_BROADCAST | IFF_RUNNING */
    reply_len = 40;
    CHECK("SIOCGIFFLAGS answers", ioctl32(FD_INET, 0x8913, 0x1000) == 0);
    CHECK("SIOCGIFFLAGS runs natively", native_cmd == 0x8913 && native_in_window);
    CHECK("the native ifreq starts with the caller's 32 bytes",
          memcmp(native_saw, U(0x1000), 16) == 0 && native_saw[32] == 0 && native_saw[39] == 0);
    CHECK("the flags come back", U(0x1010)[0] == 0x43 && memcmp(U(0x1000), "eth0", 5) == 0);
    CHECK("32 bytes come back", U(0x1020)[0] == 0xEE);

    fresh();
    memset(U(0x1000), 0, 32);
    memcpy(U(0x1000), "eth0", 5);
    U(0x1010)[0] = 1;
    CHECK("SIOCSIFFLAGS answers", ioctl32(FD_INET, 0x8914, 0x1000) == 0);
    CHECK("a setter writes nothing back", to_user_copies == 0);
    fresh();
    CHECK("an interface request from a bad pointer is EFAULT",
          ioctl32(FD_INET, 0x8913, 0x20) == -EFAULT && native_calls == 0);
    fresh();
    native_rc = -ENODEV;
    memcpy(native_reply, "zz", 3);
    reply_len = 3;
    CHECK("a failed getter writes nothing back",
          ioctl32(FD_INET, 0x8913, 0x1000) == -ENODEV && to_user_copies == 0);
    fresh();
    memset(U(0x1000), 0, 32);
    CHECK("SIOCGIWNAME goes as an interface request",
          ioctl32(FD_INET, 0x8B01, 0x1000) == 0 && native_in_window && native_cmd == 0x8B01);
    fresh();
    CHECK("SIOCSIFADDR on AF_INET takes the 32-byte ifreq",
          ioctl32(FD_INET, 0x8916, 0x1000) == 0 && native_in_window);
}

/* The native `struct ifconf`: a length, padding and a buffer pointer. */
struct ifconf_native {
    int32_t len;
    int32_t pad;
    uintptr_t buf;
};

/**
 * SIOCGIFCONF's native path with three interfaces: fill the entries of 40
 * bytes the native `ifconf` has room for, through its buffer pointer, in the
 * window, or count them all with no buffer.
 */
static void ifconf_native(uint8_t *window)
{
    static const char *const names[3] = {"eth0", "eth1", "wlan0"};
    struct ifconf_native c;
    int i;
    uint32_t used = 0;

    memcpy(&c, window, sizeof(c));
    for (i = 0; i < 3; i++) {
        if (c.buf != 0) {
            uint8_t *req = (uint8_t *)c.buf + used;

            if (c.len < 0 || used + 40 > (uint32_t)c.len) {
                break;
            }
            CHECK("the entries lie in the window",
                  req >= win_base && req + 40 <= win_base + win_len);
            memset(req, 0, 40);
            memcpy(req, names[i], strlen(names[i]) + 1);
            req[16] = 2; /* AF_INET */
            req[20] = 10;
            req[23] = (uint8_t)(i + 1);
            memset(req + 32, 0x5A, 8); /* the native tail, which i386 has not */
        }
        used += 40;
    }
    c.len = (int32_t)used;
    memcpy(window, &c.len, 4);
}

/** SIOCGIFCONF's entries of 32 bytes from native ones of 40. */
static void test_ifconf(void)
{
    struct ifconf_native c;

    fresh();
    put32(0x1000, 64); /* room for two i386 entries */
    put32(0x1004, 0x2000);
    native_hook = ifconf_native;
    CHECK("SIOCGIFCONF answers", ioctl32(FD_INET, 0x8912, 0x1000) == 0);
    memcpy(&c, native_saw, sizeof(c));
    CHECK("the native ifconf has room for two entries of 40", c.len == 80 && c.buf != 0);
    CHECK("the length counts two i386 entries", get32(0x1000) == 64);
    CHECK("the first entry's name and address",
          memcmp(U(0x2000), "eth0", 5) == 0 && U(0x2010)[0] == 2 && U(0x2017)[0] == 1);
    CHECK("the second entry at 32", memcmp(U(0x2020), "eth1", 5) == 0 && U(0x2037)[0] == 2);
    CHECK("no native tail and no third entry", U(0x2040)[0] == 0xEE);

    fresh();
    put32(0x1000, 1000);
    put32(0x1004, 0x2000);
    native_hook = ifconf_native;
    CHECK("SIOCGIFCONF with room for more answers", ioctl32(FD_INET, 0x8912, 0x1000) == 0);
    memcpy(&c, native_saw, sizeof(c));
    CHECK("the native room is cut to the interfaces", c.len == 120);
    CHECK("three i386 entries", get32(0x1000) == 96 && memcmp(U(0x2040), "wlan0", 6) == 0);
    CHECK("nothing past them", U(0x2060)[0] == 0xEE);

    fresh();
    put32(0x1000, 64);
    put32(0x1004, 0);
    native_hook = ifconf_native;
    CHECK("SIOCGIFCONF with no buffer answers", ioctl32(FD_INET, 0x8912, 0x1000) == 0);
    memcpy(&c, native_saw, sizeof(c));
    CHECK("the native ifconf has no buffer", c.buf == 0);
    CHECK("the length all entries take, in i386's", get32(0x1000) == 96);

    fresh();
    put32(0x1000, 0xFFFFFFF0u);
    put32(0x1004, 0x2000);
    native_hook = ifconf_native;
    CHECK("SIOCGIFCONF of a negative length answers", ioctl32(FD_INET, 0x8912, 0x1000) == 0);
    memcpy(&c, native_saw, sizeof(c));
    CHECK("it asks no entry and gets none", c.len == 0 && get32(0x1000) == 0);
    CHECK("it writes no entry", U(0x2000)[0] == 0xEE);
    fresh();
    CHECK("SIOCGIFCONF from a bad pointer is EFAULT",
          ioctl32(FD_INET, 0x8912, 0x40) == -EFAULT && native_calls == 0);
}

/* i386's `struct rtentry`: pad1 0, dst 4, gateway 20, genmask 36, flags 52,
 * pad2 54, pad3 56, tos 60, class 61, pad4 62, metric 64, dev 68, mtu 72,
 * window 76, irtt 80; 84 bytes. The native one: dst 8, gateway 24, genmask
 * 40, flags 56, metric 80, dev 88, mtu 96, window 104, irtt 112; 120. */
static void test_route(void)
{
    uint32_t i;

    fresh();
    memset(U(0x1000), 0, 84);
    for (i = 0; i < 48; i++) {
        U(0x1004)[i] = (uint8_t)(i + 1);
    }
    U(0x1034)[0] = 0x03;
    U(0x1040)[0] = 5;
    put32(0x1044, 0x3000);
    put32(0x1048, 1400);
    put32(0x104C, 8192);
    U(0x1050)[0] = 9;
    CHECK("SIOCADDRT answers", ioctl32(FD_INET, 0x890B, 0x1000) == 0);
    CHECK("SIOCADDRT runs natively", native_cmd == 0x890B && native_in_window);
    CHECK("rtentry's three addresses", memcmp(native_saw + 8, U(0x1004), 48) == 0);
    CHECK("rtentry's flags", native_saw[56] == 0x03);
    CHECK("rtentry's metric", native_saw[80] == 5);
    CHECK("rtentry's device pointer zero-extended", saw64(88) == 0x3000);
    CHECK("rtentry's mtu and window", saw64(96) == 1400 && saw64(104) == 8192);
    CHECK("rtentry's irtt", native_saw[112] == 9);
    fresh();
    CHECK("SIOCDELRT from a bad pointer is EFAULT",
          ioctl32(FD_INET, 0x890C, 0x30) == -EFAULT && native_calls == 0);

    /* in6_rtmsg: dst 0, src 16, gateway 32, type 48, dst_len 52, src_len
     * 54, metric 56, info 60, flags 64, ifindex 68; 72 bytes on i386, and
     * info 64, flags 72, ifindex 76; 80 natively. */
    fresh();
    for (i = 0; i < 48; i++) {
        U(0x1000)[i] = (uint8_t)(0x40 + i);
    }
    put32(0x1030, 1);
    U(0x1034)[0] = 64;
    U(0x1036)[0] = 0;
    put32(0x1038, 1024);
    put32(0x103C, 0x11);
    put32(0x1040, 0x2);
    put32(0x1044, 2);
    CHECK("SIOCADDRT on AF_INET6 answers", ioctl32(FD_INET6, 0x890B, 0x1000) == 0);
    CHECK("in6_rtmsg's addresses", memcmp(native_saw, U(0x1000), 48) == 0);
    CHECK("in6_rtmsg's type and lengths", saw32(48) == 1 && native_saw[52] == 64);
    CHECK("in6_rtmsg's metric", saw32(56) == 1024);
    CHECK("in6_rtmsg's info at 64", saw64(64) == 0x11);
    CHECK("in6_rtmsg's flags and index", saw32(72) == 2 && saw32(76) == 2);
}

/* ---- DRM ---- */

/** VERSION, GET_UNIQUE and GET_CLIENT with their 4-byte lengths, pointers
 * and longs. */
static void test_drm_core(void)
{
    CHECK("drm_version32 is 36 bytes", sizeof(struct drm_version32) == 36);
    CHECK("drm_client32 is 24 bytes", sizeof(struct drm_client32) == 24);
    CHECK("drm_wait_vblank32 is 16 bytes", sizeof(struct drm_wait_vblank32) == 16);

    /* drm_version32: major 0, minor 4, patch 8, name_len 12, name 16,
     * date_len 20, date 24, desc_len 28, desc 32. */
    fresh();
    memset(U(0x1000), 0, 36);
    put32(0x100C, 8);
    put32(0x1010, 0x2000);
    put32(0x1014, 9);
    put32(0x1018, 0x2100);
    put32(0x101C, 10);
    put32(0x1020, 0x2200);
    reply32(0, 1);
    reply32(4, 2);
    reply32(8, 3);
    reply64(16, 6);
    reply64(24, 0x2000);
    reply64(32, 8);
    reply64(40, 0x2100);
    reply64(48, 20);
    reply64(56, 0x2200);
    reply_len = 64;
    CHECK("VERSION answers", ioctl32(FD_DEV, 0xC0246400, 0x1000) == 0);
    CHECK("VERSION runs the native number", native_cmd == 0xC0406400 && native_in_window);
    CHECK("drm_version's lengths are size_t", saw64(16) == 8 && saw64(32) == 9 && saw64(48) == 10);
    CHECK("drm_version's pointers zero-extended",
          saw64(24) == 0x2000 && saw64(40) == 0x2100 && saw64(56) == 0x2200);
    CHECK("the version comes back", get32(0x1000) == 1 && get32(0x1004) == 2 && get32(0x1008) == 3);
    CHECK("the lengths come back", get32(0x100C) == 6 && get32(0x1014) == 8 && get32(0x101C) == 20);
    CHECK("the pointers stay", get32(0x1010) == 0x2000 && get32(0x1020) == 0x2200);
    CHECK("drm_version32 is 36 bytes in the caller", U(0x1024)[0] == 0xEE);
    fresh();
    memset(U(0x1000), 0, 36);
    native_rc = -EACCES;
    CHECK("a failed VERSION writes nothing back",
          ioctl32(FD_DEV, 0xC0246400, 0x1000) == -EACCES && to_user_copies == 0);
    fresh();
    CHECK("VERSION by its native number is taken too",
          ioctl32(FD_DEV, 0xC0406400, 0x1000) == 0 && native_cmd == 0xC0406400 && native_in_window);

    /* drm_unique32: len 0, pointer 4. */
    fresh();
    put32(0x1000, 32);
    put32(0x1004, 0x2000);
    reply64(0, 12);
    reply64(8, 0x2000);
    reply_len = 16;
    CHECK("GET_UNIQUE answers", ioctl32(FD_DEV, 0xC0086401, 0x1000) == 0);
    CHECK("GET_UNIQUE runs the native number", native_cmd == 0xC0106401);
    CHECK("drm_unique's length and pointer", saw64(0) == 32 && saw64(8) == 0x2000);
    CHECK("the name's length comes back", get32(0x1000) == 12 && get32(0x1004) == 0x2000);
    CHECK("drm_unique32 is 8 bytes", U(0x1008)[0] == 0xEE);

    /* drm_client32: idx 0, auth 4, pid 8, uid 12, magic 16, iocs 20. */
    fresh();
    memset(U(0x1000), 0, 24);
    put32(0x1000, 0);
    put32(0x1008, 0x55555555);
    reply32(0, 0);
    reply32(4, 1);
    reply64(8, 1234);
    reply64(16, 1000);
    reply64(24, 7);
    reply64(32, 0);
    reply_len = 40;
    CHECK("GET_CLIENT answers", ioctl32(FD_DEV, 0xC0186405, 0x1000) == 0);
    CHECK("GET_CLIENT runs the native number", native_cmd == 0xC0286405);
    CHECK("only the index goes in", saw64(8) == 0);
    CHECK("drm_client's record comes back in 32 bits",
          get32(0x1004) == 1 && get32(0x1008) == 1234 && get32(0x100C) == 1000 &&
              get32(0x1010) == 7 && get32(0x1014) == 0);
    CHECK("drm_client32 is 24 bytes", U(0x1018)[0] == 0xEE);

    fresh();
    CHECK("GET_STATS answers", ioctl32(FD_DEV, 0x807C6406, 0x1000) == 0 && native_calls == 0);
    CHECK("GET_STATS zeroes drm_stats32's 124 bytes",
          U(0x1000)[0] == 0 && U(0x1000 + 123)[0] == 0 && U(0x1000 + 124)[0] == 0xEE);
    fresh();
    CHECK("SET_UNIQUE is EINVAL",
          ioctl32(FD_DEV, 0x40086410, 0x1000) == -EINVAL && native_calls == 0);
    fresh();
    CHECK("UPDATE_DRAW is 0", ioctl32(FD_DEV, 0x4014643F, 0x1000) == 0 && native_calls == 0);
}

/** WAIT_VBLANK, ADDFB2, GETFB2, a driver's request and a passing one. */
static void test_drm_mode(void)
{
    uint32_t i;

    /* drm_wait_vblank32: type 0, sequence 4, signal or seconds 8,
     * microseconds 12. */
    fresh();
    put32(0x1000, 0x1);
    put32(0x1004, 1);
    put32(0x1008, 0x77);
    put32(0x100C, 0);
    reply32(0, 0x1);
    reply32(4, 100);
    reply64(8, 0x12345);
    reply64(16, 678);
    reply_len = 24;
    native_rc = -EINTR;
    CHECK("WAIT_VBLANK returns the native result", ioctl32(FD_DEV, 0xC010643A, 0x1000) == -EINTR);
    CHECK("WAIT_VBLANK runs the native number", native_cmd == 0xC018643A);
    CHECK("the request goes in", saw32(0) == 1 && saw32(4) == 1 && saw64(8) == 0x77);
    CHECK("the reply comes back whatever the result",
          get32(0x1004) == 100 && get32(0x1008) == 0x12345 && get32(0x100C) == 678);
    CHECK("drm_wait_vblank32 is 16 bytes", U(0x1010)[0] == 0xEE);

    /* drm_mode_fb_cmd232: fb_id 0, width 4, height 8, format 12, flags
     * 16, handles 20, pitches 36, offsets 52, modifier 68; 100 bytes. */
    fresh();
    memset(U(0x1000), 0, 100);
    put32(0x1004, 640);
    put32(0x1008, 480);
    put32(0x100C, 0x34325258);
    put32(0x1010, 2);
    put32(0x1014, 1);
    put32(0x1024, 2560);
    for (i = 0; i < 4; i++) {
        put32(0x1044 + i * 8, 0x11 + i);
        put32(0x1048 + i * 8, 0x01000000);
    }
    reply32(0, 42);
    reply_len = 4;
    CHECK("ADDFB2 answers", ioctl32(FD_DEV, 0xC06464B8, 0x1000) == 0);
    CHECK("ADDFB2 runs the native number", native_cmd == 0xC06864B8 && native_in_window);
    CHECK("drm_mode_fb_cmd2's first 68 bytes",
          saw32(0) == 0 && memcmp(native_saw + 4, U(0x1004), 64) == 0);
    CHECK("the modifiers at 72 natively",
          saw64(72) == 0x0100000000000011ull && saw64(96) == 0x0100000000000014ull);
    CHECK("the framebuffer's id comes back", get32(0x1000) == 42);
    CHECK("nothing else is written", get32(0x1004) == 640 && U(0x1064)[0] == 0xEE);
    fresh();
    memset(U(0x1000), 0, 100);
    native_rc = -EINVAL;
    CHECK("a failed ADDFB2 writes nothing back",
          ioctl32(FD_DEV, 0xC06464B8, 0x1000) == -EINVAL && to_user_copies == 0);

    fresh();
    memset(U(0x1000), 0, 100);
    put32(0x1000, 42);
    reply32(0, 42);
    reply32(4, 800);
    reply32(8, 600);
    reply64(72, 0x0100000000000001ull);
    reply64(96, 0x0100000000000004ull);
    reply_len = 104;
    CHECK("GETFB2 answers", ioctl32(FD_DEV, 0xC06464CE, 0x1000) == 0);
    CHECK("GETFB2 runs the native number", native_cmd == 0xC06864CE && saw32(0) == 42);
    CHECK("its fields come back", get32(0x1004) == 800 && get32(0x1008) == 600);
    CHECK("its modifiers come back at 68", get32(0x1044) == 1 && get32(0x1048) == 0x01000000 &&
                                               get32(0x105C) == 4 && get32(0x1060) == 0x01000000);
    CHECK("GETFB2's structure is 100 bytes", U(0x1064)[0] == 0xEE);

    fresh();
    CHECK("a driver's request goes to the GPU translations",
          ioctl32(FD_DEV, 0xC0106440, 0x1000) == -ENOTTY && native_calls == 0);
    fresh();
    native_rc = 3;
    CHECK("GET_CAP passes", ioctl32(FD_DEV, 0xC010640C, 0x1000) == 3);
    CHECK("GET_CAP keeps its number and pointer", native_cmd == 0xC010640C && native_arg == 0x1000);
    fresh();
    CHECK("OBJ_GETPROPERTIES of 28 bytes passes as numbered",
          ioctl32(FD_DEV, 0xC01C64B9, 0x1000) == 0 && native_cmd == 0xC01C64B9);
    fresh();
    CHECK("dma-buf goes to the GPU translations", ioctl32(FD_DEV, 0x40086200, 0x1000) == -ENOTTY);
    CHECK("sync_file goes to the GPU translations", ioctl32(FD_DEV, 0xC0303E03, 0x1000) == -ENOTTY);
    CHECK("udmabuf goes to the GPU translations", ioctl32(FD_DEV, 0x40187542, 0x1000) == -ENOTTY);
    CHECK("none of them runs natively", native_calls == 0);
}

int main(void)
{
    entries[FD_DEV].type = FD_TYPE_DEVICE;
    entries[FD_INET].type = FD_TYPE_SOCKET;
    entries[FD_INET6].type = FD_TYPE_SOCKET;
    inet6_sock.family = AF_INET6;
    entries[FD_INET6].sock = &inet6_sock;
    entries[FD_CON].type = FD_TYPE_CONSOLE;
    entries[FD_PATHFD].type = FD_TYPE_FILE;
    entries[FD_PATHFD].flags = FD_FLAG_PATH;
    proc.fds = &table;
    cpu_table[0].current = &proc;

    test_path();
    test_pass();
    test_fs_attr();
    test_block();
    test_loop();
    test_evdev();
    test_audio();
    test_kvm();
    test_net();
    test_ifconf();
    test_route();
    test_drm_core();
    test_drm_mode();
    if (fails) {
        printf("ia32-ioctl-test: %d failures\n", fails);
        return 1;
    }
    printf("ia32-ioctl-test: ok\n");
    return 0;
}
