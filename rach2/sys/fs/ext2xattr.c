/* The checked ext2 external attribute block layout. */
#include "ext2xattr.h"
#include "fatcommon.h"
#include "../lib/kerrno.h"
#include "../lib/kstring.h"

/** The little-endian 16-bit field at `p`. */
static uint32_t le16(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

/** The little-endian 32-bit field at `p`. */
static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

#define LE16_PUT(p, v)                           \
    do {                                         \
        uint32_t put_ = (v);                     \
        (p)[0] = (uint8_t)(put_ & 0xFFu);        \
        (p)[1] = (uint8_t)((put_ >> 8) & 0xFFu); \
    } while (0)
#define LE32_PUT(p, v)                            \
    do {                                          \
        uint32_t put_ = (v);                      \
        (p)[0] = (uint8_t)(put_ & 0xFFu);         \
        (p)[1] = (uint8_t)((put_ >> 8) & 0xFFu);  \
        (p)[2] = (uint8_t)((put_ >> 16) & 0xFFu); \
        (p)[3] = (uint8_t)((put_ >> 24) & 0xFFu); \
    } while (0)

#define XA_MAGIC 0xEA020000u
#define XA_HEAD  32u
#define XA_ENTRY 16u

/* An absent block reads as one whose first entry is the terminator. */
static const uint8_t xa_empty[XA_HEAD + 4];

/** Round a checked byte count up to the format's four-byte alignment. */
static uint32_t xa_pad(uint32_t n)
{
    return (n + 3u) & ~3u;
}

/** Compare unsigned bytes without relying on a hosted library. */
static int xa_bytes(const void *a, const void *b, uint32_t n)
{
    const uint8_t *x = a, *y = b;
    uint32_t i;
    for (i = 0; i < n; i++)
        if (x[i] != y[i])
            return (int)x[i] - (int)y[i];
    return 0;
}

/** Compare entry keys in Linux's index, name-length, name order. */
static int xa_cmp(const uint8_t *a, const uint8_t *b)
{
    int d = (int)a[1] - (int)b[1];
    if (!d)
        d = (int)a[0] - (int)b[0];
    return d ? d : xa_bytes(a + XA_ENTRY, b + XA_ENTRY, a[0]);
}

/** Check inline values after the complete entry table has been checked.
 * `end` is the first byte after its terminator. Values may not overlap. */
static int xa_values(const uint8_t *b, uint32_t bytes, uint32_t end)
{
    uint32_t p, q, off, size;

    for (p = XA_HEAD; le32(b + p); p += xa_pad(XA_ENTRY + b[p])) {
        off = le16(b + p + 2);
        size = le32(b + p + 8);
        if (le32(b + p + 4) || size > bytes || off > bytes - size ||
            (size && ((off & 3u) || off < end || xa_pad(size) > bytes - off)))
            return -EIO;
        for (q = XA_HEAD; q < p; q += xa_pad(XA_ENTRY + b[q])) {
            uint32_t other = le16(b + q + 2), len = le32(b + q + 8);
            if (size && len && off < other + xa_pad(len) && other < off + xa_pad(size))
                return -EIO;
        }
    }
    return 0;
}

/** Validate a complete external block before entry or value access. */
int ext2_xattr_check(const uint8_t *b, uint32_t bytes)
{
    uint32_t p = XA_HEAD, q, prev = 0;

    if (bytes < 1024 || bytes > 4096 || le32(b) != XA_MAGIC || le32(b + 4) == 0 ||
        le32(b + 4) > 1024 || le32(b + 8) != 1)
        return -EIO;
    while (p <= bytes - 4 && le32(b + p)) {
        if (p > bytes - XA_ENTRY || b[p + 1] == 0 || xa_pad(XA_ENTRY + b[p]) > bytes - p - 4)
            return -EIO;
        if (prev && xa_cmp(b + prev, b + p) >= 0)
            return -EIO;
        for (q = 0; q < b[p]; q++)
            if (!b[p + XA_ENTRY + q])
                return -EIO;
        prev = p;
        p += xa_pad(XA_ENTRY + b[p]);
    }
    if (p > bytes - 4)
        return -EIO;
    return xa_values(b, bytes, p + 4);
}

/** Read one value from a checked block. */
int ext2_xattr_read(const uint8_t *b, uint32_t bytes, int index, const char *name, void *value,
                    uint32_t size)
{
    uint32_t p, n, len = (uint32_t)k_strlen(name);
    int rc;

    if (!b)
        return -ENODATA;
    rc = ext2_xattr_check(b, bytes);
    if (rc)
        return rc;
    for (p = XA_HEAD; le32(b + p); p += xa_pad(XA_ENTRY + b[p])) {
        if (b[p + 1] != (uint32_t)index || len != b[p] || xa_bytes(name, b + p + XA_ENTRY, len))
            continue;
        n = le32(b + p + 8);
        if (size && size < n)
            return -ERANGE;
        if (size && n)
            k_memcpy(value, b + le16(b + p + 2), n);
        return (int)n;
    }
    return -ENODATA;
}

/** The full name's prefix of the entries of index `index`, or NULL for an
 * index that is not listed. An ACL's name is its prefix alone. */
static const char *xa_prefix(uint32_t index, int acl)
{
    if (index == EXT2_XATTR_USER)
        return "user.";
    if (acl && index == EXT2_XATTR_ACL_ACCESS)
        return "system.posix_acl_access";
    if (acl && index == EXT2_XATTR_ACL_DEFAULT)
        return "system.posix_acl_default";
    return NULL;
}

/** List the names of a checked block. */
int ext2_xattr_list(const uint8_t *b, uint32_t bytes, int acl, void *value, uint32_t size)
{
    uint8_t *out = value;
    uint32_t p, used = 0;
    int rc;

    if (!b)
        return 0;
    rc = ext2_xattr_check(b, bytes);
    if (rc)
        return rc;
    for (p = XA_HEAD; le32(b + p); p += xa_pad(XA_ENTRY + b[p])) {
        const char *prefix = xa_prefix(b[p + 1], acl);
        uint32_t pre, n = b[p];

        if (!prefix)
            continue;
        pre = (uint32_t)k_strlen(prefix);
        if (size && used + pre + n + 1 > size)
            return -ERANGE;
        if (size) {
            k_memcpy(out + used, prefix, pre);
            k_memcpy(out + used + pre, b + p + XA_ENTRY, n);
            out[used + pre + n] = 0;
        }
        used += pre + n + 1;
    }
    return (int)used;
}

/** Append one entry and value, leaving space for the zero terminator.
 * Updates the entry cursor and descending value cursor, or returns -ENOSPC. */
static int xa_add(uint8_t *out, uint32_t *pos, uint32_t *tail, const uint8_t *key,
                  const void *value, uint32_t size)
{
    uint32_t len = xa_pad(XA_ENTRY + key[0]), hash = 0, i, vh;
    uint8_t *e = out + *pos;

    if (size > *tail || xa_pad(size) > *tail || len + 4 > *tail - xa_pad(size) ||
        *pos > *tail - xa_pad(size) - len - 4)
        return -ENOSPC;
    *tail -= xa_pad(size);
    k_memcpy(e, key, XA_ENTRY + key[0]);
    LE16_PUT(e + 2, size ? *tail : 0);
    LE32_PUT(e + 4, 0);
    LE32_PUT(e + 8, size);
    if (size)
        k_memcpy(out + *tail, value, size);
    for (i = 0; i < key[0]; i++)
        hash = (hash << 5) ^ (hash >> 27) ^ key[XA_ENTRY + i];
    for (i = 0; i < xa_pad(size); i += 4)
        hash = (hash << 16) ^ (hash >> 16) ^ le32(out + *tail + i);
    LE32_PUT(e + 12, hash);
    vh = le32(out + 12);
    LE32_PUT(out + 12, (vh << 16) ^ (vh >> 16) ^ hash);
    *pos += len;
    return 0;
}

/** Pack checked entries around a changed key, with Linux's hashes.
 * `key` is the new entry header and name; `out` is distinct from `b`. */
static int xa_pack(const uint8_t *b, uint8_t *out, uint32_t bytes, const uint8_t *key,
                   const struct vfs_xattr *arg)
{
    const void *value = arg->op == VFS_XATTR_REMOVE ? NULL : arg->value;
    uint32_t size = arg->size, p, pos = XA_HEAD, tail = bytes;
    int inserted = 0, rc;

    k_memset(out, 0, bytes);
    LE32_PUT(out, XA_MAGIC);
    LE32_PUT(out + 4, 1);
    LE32_PUT(out + 8, 1);
    for (p = XA_HEAD; le32(b + p); p += xa_pad(XA_ENTRY + b[p])) {
        int cmp = xa_cmp(key, b + p);
        if (!inserted && cmp <= 0) {
            if (value) {
                rc = xa_add(out, &pos, &tail, key, value, size);
                if (rc)
                    return rc;
            }
            inserted = 1;
        }
        if (!cmp)
            continue;
        rc = xa_add(out, &pos, &tail, b + p, b + (le32(b + p + 8) ? le16(b + p + 2) : 0),
                    le32(b + p + 8));
        if (rc)
            return rc;
    }
    if (!inserted && value) {
        rc = xa_add(out, &pos, &tail, key, value, size);
        if (rc)
            return rc;
    }
    /* Linux leaves a block unshareable if any entry's hash is zero. */
    for (p = XA_HEAD; p < pos; p += xa_pad(XA_ENTRY + out[p]))
        if (!le32(out + p + 12)) {
            LE32_PUT(out + 12, 0);
            break;
        }
    return pos != XA_HEAD;
}

/** Build a private replacement, preserving the other entries. */
int ext2_xattr_edit(const uint8_t *b, uint8_t *out, uint32_t bytes, int index,
                    const struct vfs_xattr *arg)
{
    const char *name = arg->name;
    const void *value = arg->op == VFS_XATTR_REMOVE ? NULL : arg->value;
    int flags = arg->flags;
    uint8_t key[XA_ENTRY + 255];
    uint32_t len = (uint32_t)k_strlen(name), p;
    int rc, found = 0;

    if ((!len && index == EXT2_XATTR_USER) || len > 255 || index < 1 || index > 255 || flags < 0 ||
        (flags & ~3))
        return -EINVAL;
    if (bytes < 1024 || bytes > 4096)
        return -EIO;
    if (b) {
        rc = ext2_xattr_check(b, bytes);
        if (rc)
            return rc;
    } else {
        b = xa_empty;
    }
    k_memset(key, 0, sizeof(key));
    key[0] = (uint8_t)len;
    key[1] = (uint8_t)index;
    k_memcpy(key + XA_ENTRY, name, len);
    for (p = XA_HEAD; le32(b + p); p += xa_pad(XA_ENTRY + b[p]))
        if (!xa_cmp(key, b + p))
            found = 1;
    if (found && (flags & 1))
        return -EEXIST;
    if (!found && (!value || (flags & 2)))
        return -ENODATA;
    return xa_pack(b, out, bytes, key, arg);
}
