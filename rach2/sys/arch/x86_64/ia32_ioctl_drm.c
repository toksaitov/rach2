/*
 * ia32_ioctl_drm.c: the i386 forms of the DRM core's ioctls, as Linux's
 * drm_ioc32.c translates them, for the compat ioctl path (ia32_ioctl.c).
 */

#include "ia32.h"

#include "../../drm/uapi.h"
#include "../../lib/kstring.h"
#include "../../mm/usercopy.h"
#include "../../syscall.h"

/* The numbers of the core's requests Linux translates, by which a request
 * of any size and direction is taken, as drm_compat_ioctl takes it. */
#define NR_VERSION     0x00u
#define NR_GET_UNIQUE  0x01u
#define NR_GET_CLIENT  0x05u
#define NR_GET_STATS   0x06u
#define NR_SET_UNIQUE  0x10u
#define NR_WAIT_VBLANK 0x3Au
#define NR_UPDATE_DRAW 0x3Fu
#define NR_ADDFB2      0xB8u
#define NR_GETFB2      0xCEu

/* drm_version32_t: the lengths and pointers in 32 bits, 36 bytes. */
struct drm_version32 {
    int32_t version_major;
    int32_t version_minor;
    int32_t version_patchlevel;
    uint32_t name_len;
    uint32_t name;
    uint32_t date_len;
    uint32_t date;
    uint32_t desc_len;
    uint32_t desc;
};

/* drm_client32_t: its longs in 32 bits, 24 bytes. */
struct drm_client32 {
    int32_t idx;
    int32_t auth;
    uint32_t pid;
    uint32_t uid;
    uint32_t magic;
    uint32_t iocs;
};

/* drm_wait_vblank32_t: the request's signal and the reply's time in 32
 * bits, 16 bytes. */
struct drm_wait_vblank32 {
    uint32_t type;
    uint32_t sequence;
    uint32_t signal_or_sec; /* the request's signal, the reply's seconds */
    int32_t usec;           /* the reply's microseconds */
};

/* drm_mode_fb_cmd232_t: the modifiers at 68, where i386 aligns a 64-bit
 * member to 4 bytes, 100 bytes. */
#define FB_CMD2_32_MODIFIER 68u
#define FB_CMD2_32_LEN      100u

/** DRM_IOCTL_VERSION: the lengths and buffers in 32 bits; the version and
 * the strings' lengths are copied back once the call succeeds. */
static long version(unsigned int fd, unsigned long arg)
{
    struct drm_version32 v32;
    struct drm_version v;
    long rc;

    if (copy_from_user(&v32, (const void __user *)arg, sizeof(v32)) != 0) {
        return -EFAULT;
    }
    k_memset(&v, 0, sizeof(v));
    v.name_len = v32.name_len;
    v.name = (char __user *)(uintptr_t)v32.name;
    v.date_len = v32.date_len;
    v.date = (char __user *)(uintptr_t)v32.date;
    v.desc_len = v32.desc_len;
    v.desc = (char __user *)(uintptr_t)v32.desc;
    rc = ia32_ioctl_native(fd, DRM_IOCTL_VERSION, &v, sizeof(v));
    if (rc != 0) {
        return rc;
    }
    v32.version_major = v.version_major;
    v32.version_minor = v.version_minor;
    v32.version_patchlevel = v.version_patchlevel;
    v32.name_len = (uint32_t)v.name_len;
    v32.date_len = (uint32_t)v.date_len;
    v32.desc_len = (uint32_t)v.desc_len;
    return copy_to_user((void __user *)arg, &v32, sizeof(v32)) != 0 ? -EFAULT : 0;
}

/** DRM_IOCTL_GET_UNIQUE: the length and buffer in 32 bits; the name's
 * length is copied back once the call succeeds. */
static long get_unique(unsigned int fd, unsigned long arg)
{
    uint32_t u32[2];
    struct drm_unique u;
    long rc;

    if (copy_from_user(u32, (const void __user *)arg, sizeof(u32)) != 0) {
        return -EFAULT;
    }
    k_memset(&u, 0, sizeof(u));
    u.unique_len = u32[0];
    u.unique = (char __user *)(uintptr_t)u32[1];
    rc = ia32_ioctl_native(fd, DRM_IOCTL_GET_UNIQUE, &u, sizeof(u));
    if (rc != 0) {
        return rc;
    }
    u32[0] = (uint32_t)u.unique_len;
    return copy_to_user((void __user *)arg, u32, sizeof(u32)) != 0 ? -EFAULT : 0;
}

/** DRM_IOCTL_GET_CLIENT: the index asked for goes in, and the record comes
 * back with its longs in 32 bits once the call succeeds. */
static long get_client(unsigned int fd, unsigned long arg)
{
    struct drm_client32 c32;
    struct drm_client c;
    long rc;

    if (copy_from_user(&c32, (const void __user *)arg, sizeof(c32)) != 0) {
        return -EFAULT;
    }
    k_memset(&c, 0, sizeof(c));
    c.idx = c32.idx;
    rc = ia32_ioctl_native(fd, DRM_IOCTL_GET_CLIENT, &c, sizeof(c));
    if (rc != 0) {
        return rc;
    }
    c32.idx = c.idx;
    c32.auth = c.auth;
    c32.pid = (uint32_t)c.pid;
    c32.uid = (uint32_t)c.uid;
    c32.magic = (uint32_t)c.magic;
    c32.iocs = (uint32_t)c.iocs;
    return copy_to_user((void __user *)arg, &c32, sizeof(c32)) != 0 ? -EFAULT : 0;
}

/** DRM_IOCTL_GET_STATS: the 124 bytes of drm_stats32_t zeroed, as Linux's
 * compat_drm_getstats answers for a call it no longer serves. */
static long get_stats(unsigned long arg)
{
    static const uint8_t zero[124];

    return copy_to_user((void __user *)arg, zero, sizeof(zero)) != 0 ? -EFAULT : 0;
}

/** DRM_IOCTL_WAIT_VBLANK: the request in, the reply back with its time in
 * 32 bits whatever the call returned, as Linux's compat_drm_wait_vblank
 * copies it. */
static long wait_vblank(unsigned int fd, unsigned long arg)
{
    struct drm_wait_vblank32 w32;
    union drm_wait_vblank w;
    long rc;

    if (copy_from_user(&w32, (const void __user *)arg, sizeof(w32)) != 0) {
        return -EFAULT;
    }
    k_memset(&w, 0, sizeof(w));
    w.request.type = (enum drm_vblank_seq_type)w32.type;
    w.request.sequence = w32.sequence;
    w.request.signal = w32.signal_or_sec;
    rc = ia32_ioctl_native(fd, DRM_IOCTL_WAIT_VBLANK, &w, sizeof(w));
    w32.type = (uint32_t)w.reply.type;
    w32.sequence = w.reply.sequence;
    w32.signal_or_sec = (uint32_t)w.reply.tval_sec;
    w32.usec = (int32_t)w.reply.tval_usec;
    if (copy_to_user((void __user *)arg, &w32, sizeof(w32)) != 0) {
        return -EFAULT;
    }
    return rc;
}

/** DRM_IOCTL_MODE_ADDFB2: the 68 bytes before the modifiers are the native
 * structure's, and the modifiers follow at 68; the new framebuffer's id is
 * copied back once the call succeeds. */
static long addfb2(unsigned int fd, unsigned long arg)
{
    struct drm_mode_fb_cmd2 f;
    long rc;

    k_memset(&f, 0, sizeof(f));
    if (copy_from_user(&f, (const void __user *)arg, FB_CMD2_32_MODIFIER) != 0 ||
        copy_from_user(f.modifier, (const void __user *)(arg + FB_CMD2_32_MODIFIER),
                       sizeof(f.modifier)) != 0) {
        return -EFAULT;
    }
    rc = ia32_ioctl_native(fd, DRM_IOCTL_MODE_ADDFB2, &f, sizeof(f));
    if (rc != 0) {
        return rc;
    }
    return copy_to_user((void __user *)arg, &f.fb_id, sizeof(f.fb_id)) != 0 ? -EFAULT : 0;
}

/**
 * DRM_IOCTL_MODE_GETFB2: the framebuffer's id in, and the structure back
 * with the modifiers at 68 once the call succeeds. Linux 6.12 passes this
 * call untranslated, which leaves a compat caller the modifiers 4 bytes
 * late and the last one cut (dev-log/79-ia32/upstream.md); its layout is
 * ADDFB2's, which Linux translates.
 */
static long getfb2(unsigned int fd, unsigned long arg)
{
    struct drm_mode_fb_cmd2 f;
    uint8_t f32[FB_CMD2_32_LEN];
    long rc;

    k_memset(&f, 0, sizeof(f));
    if (copy_from_user(f32, (const void __user *)arg, sizeof(f32)) != 0) {
        return -EFAULT;
    }
    k_memcpy(&f, f32, FB_CMD2_32_MODIFIER);
    k_memcpy(f.modifier, f32 + FB_CMD2_32_MODIFIER, sizeof(f.modifier));
    rc = ia32_ioctl_native(fd, DRM_IOCTL_MODE_GETFB2, &f, sizeof(f));
    if (rc != 0) {
        return rc;
    }
    k_memcpy(f32, &f, FB_CMD2_32_MODIFIER);
    k_memcpy(f32 + FB_CMD2_32_MODIFIER, f.modifier, sizeof(f.modifier));
    return copy_to_user((void __user *)arg, f32, sizeof(f32)) != 0 ? -EFAULT : 0;
}

/**
 * The DRM requests of a compat caller, by their number whatever the size
 * and direction they are encoded with, as Linux's drm_compat_ioctl takes
 * them: VERSION, GET_UNIQUE, GET_CLIENT, WAIT_VBLANK, ADDFB2 and GETFB2
 * translated; GET_STATS zeroed and SET_UNIQUE -EINVAL, the calls Linux no
 * longer serves; UPDATE_DRAW 0, as Linux's compat handler returns it. A
 * driver's own request, numbered from DRM_COMMAND_BASE, goes to the GPU
 * drivers' translations (ia32_ioctl_gpu.c). Every other core request
 * passes: its structure has one layout in both ABIs, or differs only by tail
 * padding, which the core's dispatcher takes by the size the number
 * carries.
 */
long ia32_ioctl_drm(unsigned int fd, unsigned int cmd, unsigned long arg)
{
    uint32_t nr = UAPI_IOC_NR(cmd);

    if (nr >= DRM_COMMAND_BASE && nr < DRM_COMMAND_END) {
        return ia32_ioctl_gpu(fd, cmd, arg);
    }
    switch (nr) {
    case NR_VERSION:
        return version(fd, arg);
    case NR_GET_UNIQUE:
        return get_unique(fd, arg);
    case NR_GET_CLIENT:
        return get_client(fd, arg);
    case NR_GET_STATS:
        return get_stats(arg);
    case NR_SET_UNIQUE:
        return -EINVAL;
    case NR_WAIT_VBLANK:
        return wait_vblank(fd, arg);
    case NR_UPDATE_DRAW:
        return 0;
    case NR_ADDFB2:
        return addfb2(fd, arg);
    case NR_GETFB2:
        return getfb2(fd, arg);
    default:
        return ioctl_fd((int)fd, cmd, (uintptr_t)arg);
    }
}
