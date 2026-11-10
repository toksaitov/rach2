/*
 * i915_user_extensions.c: the walk of a chain of extensions that the
 * driver's calls take from user memory, after Linux 6.12's
 * i915_user_extensions.c.
 */

#include "i915.h"
#include "i915_drm.h"

#include "../../lib/kerrno.h"
#include "../../mm/usercopy.h"

/* The user address a 64-bit field of a call names. */
#define I915_USER_PTR(v) ((void __user *)(uintptr_t)(v))

/* How many extensions a chain may hold, Linux's. */
#define I915_MAX_EXTENSIONS 512

/** 0 when the `size` bytes at the user address `at` are zero, -EINVAL when
 * not, -EFAULT when they cannot be read, as Linux's check_user_mbz. */
int32_t i915_check_user_mbz(uintptr_t at, size_t size)
{
    uint8_t v[8];
    size_t i;

    if (copy_from_user(v, I915_USER_PTR(at), size) != 0) {
        return -EFAULT;
    }
    for (i = 0; i < size; i++) {
        if (v[i]) {
            return -EINVAL;
        }
    }
    return 0;
}

/** The walk i915.h declares: each extension's words that must be 0 are
 * read before its name, and its next link after its handler, as Linux's. */
int32_t i915_user_extensions(uint64_t ext, const i915_ext_fn *tbl, uint32_t count, void *data)
{
    struct i915_user_extension e;
    uint32_t depth = I915_MAX_EXTENSIONS;
    int32_t err;
    int i;

    while (ext) {
        uintptr_t at = (uintptr_t)ext;

        if (depth-- == 0) {
            return -E2BIG;
        }
        err = i915_check_user_mbz(at + 12u, 4);
        for (i = 0; i < 4 && err == 0; i++) {
            err = i915_check_user_mbz(at + 16u + 4u * (uint32_t)i, 4);
        }
        if (err == 0) {
            err = copy_from_user(&e.name, I915_USER_PTR(at + 8u), 4) != 0 ? -EFAULT : 0;
        }
        if (err < 0) {
            return err;
        }
        err = e.name < count && tbl[e.name] ? tbl[e.name](at, data) : -EINVAL;
        if (err < 0) {
            return err;
        }
        if (copy_from_user(&e.next_extension, I915_USER_PTR(at), 8) != 0 ||
            e.next_extension > (uint64_t)(uintptr_t)-1) {
            return -EFAULT;
        }
        ext = e.next_extension;
    }
    return 0;
}
