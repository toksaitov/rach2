/*
 * ext2_acl.c: POSIX ACLs on ext2, as Linux's fs/ext2/acl.c keeps them over
 * the rules of fs/posix_acl.c: their on-disk and xattr forms, the permission
 * check, inheritance at a creation and the update at a chmod.
 */

#include "ext2_internal.h"
#include "permission.h"

/* The tags of an ACL's entries and its permission bits, Linux's values. */
#define ACL_USER_OBJ  0x01u
#define ACL_USER      0x02u
#define ACL_GROUP_OBJ 0x04u
#define ACL_GROUP     0x08u
#define ACL_MASK      0x10u
#define ACL_OTHER     0x20u
#define ACL_RWX       7u

/* The id an entry without one carries in the xattr form, and the versions
 * of the two forms' headers. */
#define ACL_UNDEFINED_ID  0xFFFFFFFFu
#define ACL_DISK_VERSION  1u
#define ACL_XATTR_VERSION 2u

/** Free the entries of `a` and leave it empty, which is no ACL. */
static void acl_free(struct ext2_acl *a)
{
    kfree(a->e);
    a->e = NULL;
    a->count = 0;
}

/** Make `a` an ACL of `count` entries, uninitialised; 0 or -ENOMEM. */
static int acl_alloc(struct ext2_acl *a, uint32_t count)
{
    a->e = kmalloc(count * sizeof(*a->e));
    a->count = a->e ? count : 0;
    return a->e ? 0 : -ENOMEM;
}

/** Whether `tag` is one of the four entries of the short on-disk form. */
static int acl_short(uint32_t tag)
{
    return tag == ACL_USER_OBJ || tag == ACL_GROUP_OBJ || tag == ACL_MASK || tag == ACL_OTHER;
}

/** The entries of an on-disk value of `size` bytes, as Linux's
 * `ext2_acl_count` reckons them, four short ones before the long ones, or
 * -1 for a size no such layout has. */
static int acl_disk_count(uint32_t size)
{
    size -= 4;
    if (size < 4 * 4)
        return size % 4 ? -1 : (int)(size / 4);
    return (size - 4 * 4) % 8 ? -1 : (int)((size - 4 * 4) / 8 + 4);
}

/**
 * Decode the on-disk value `v` of `size` bytes into `a`, as Linux's
 * `ext2_acl_from_disk`: a version-1 header, then four-byte entries for the
 * owner, the owning group, the mask and the others and eight-byte ones,
 * with an id, for the named users and groups, as many as `acl_disk_count`
 * gives, ending at the value's end. A value of a header alone is no ACL,
 * with `a` empty. 0, -EINVAL for a malformed value, or -ENOMEM.
 */
static int acl_from_disk(const uint8_t *v, uint32_t size, struct ext2_acl *a)
{
    uint32_t i, p = 4;
    int count, rc;

    a->count = 0;
    a->e = NULL;
    if (size < 4 || ext2_le32(v) != ACL_DISK_VERSION)
        return -EINVAL;
    count = acl_disk_count(size);
    if (count <= 0)
        return count ? -EINVAL : 0;
    rc = acl_alloc(a, (uint32_t)count);
    if (rc)
        return rc;
    for (i = 0; i < a->count; i++) {
        struct ext2_acl_entry *e = &a->e[i];

        if (size - p < 4)
            break;
        e->tag = ext2_le16(v + p);
        e->perm = ext2_le16(v + p + 2);
        e->id = ACL_UNDEFINED_ID;
        p += 4;
        if (acl_short(e->tag))
            continue;
        if ((e->tag != ACL_USER && e->tag != ACL_GROUP) || size - p < 4)
            break;
        e->id = ext2_le32(v + p);
        p += 4;
    }
    if (i != a->count || p != size) {
        acl_free(a);
        return -EINVAL;
    }
    return 0;
}

/** The bytes the on-disk form of `a` takes. */
static uint32_t acl_disk_size(const struct ext2_acl *a)
{
    uint32_t i, size = 4;

    for (i = 0; i < a->count; i++)
        size += acl_short(a->e[i].tag) ? 4 : 8;
    return size;
}

/** Encode `a`, of valid tags, in the on-disk form into `out`, which holds
 * `acl_disk_size` bytes. */
static void acl_to_disk(const struct ext2_acl *a, uint8_t *out)
{
    uint32_t i, p = 4;

    LE32_PUT(out, ACL_DISK_VERSION);
    for (i = 0; i < a->count; i++) {
        LE16_PUT(out + p, a->e[i].tag);
        LE16_PUT(out + p + 2, a->e[i].perm);
        if (acl_short(a->e[i].tag)) {
            p += 4;
            continue;
        }
        LE32_PUT(out + p + 4, a->e[i].id);
        p += 8;
    }
}

/** The kernel's id of the id `id` of the caller's user namespace, or -1
 * where its map does not cover it, as Linux's `make_kuid` answers. */
static int64_t acl_id_in(uint32_t id, uint32_t tag)
{
    process_t *p = current_proc;

    if (id == ACL_UNDEFINED_ID)
        return -1;
    if (!p || !p->userns)
        return id;
    return tag == ACL_USER ? ns_uid_to_kernel(p->userns, id) : ns_gid_to_kernel(p->userns, id);
}

/** The id in the caller's user namespace of the kernel's id `kid`, or -1
 * where its map does not cover it, as Linux's `from_kuid` answers. */
static uint32_t acl_id_out(uint32_t kid, uint32_t tag)
{
    process_t *p = current_proc;
    uint32_t id;

    if (!p || !p->userns)
        return kid;
    id = tag == ACL_USER ? ns_uid_from_kernel(p->userns, kid) : ns_gid_from_kernel(p->userns, kid);
    return acl_id_in(id, tag) == (int64_t)kid ? id : ACL_UNDEFINED_ID;
}

/**
 * Decode the xattr form `v` of `size` bytes, as setfacl writes it, into `a`,
 * as Linux's `posix_acl_from_xattr`: a version-2 header, then eight-byte
 * entries of a tag, the permissions and an id, a named user's or group's in
 * the caller's user namespace. A header alone is no ACL. 0, -EOPNOTSUPP for
 * another version, -EINVAL for a malformed value, a tag Linux does not know
 * or an id the namespace does not map, or -ENOMEM.
 */
static int acl_from_xattr(const uint8_t *v, uint32_t size, struct ext2_acl *a)
{
    uint32_t i;
    int rc;

    a->count = 0;
    a->e = NULL;
    if (size < 4)
        return -EINVAL;
    if (ext2_le32(v) != ACL_XATTR_VERSION)
        return -EOPNOTSUPP;
    if ((size - 4) % 8)
        return -EINVAL;
    if (size == 4)
        return 0;
    rc = acl_alloc(a, (size - 4) / 8);
    if (rc)
        return rc;
    for (i = 0; i < a->count; i++) {
        struct ext2_acl_entry *e = &a->e[i];
        const uint8_t *p = v + 4 + i * 8;
        int64_t kid;

        e->tag = ext2_le16(p);
        e->perm = ext2_le16(p + 2);
        e->id = ACL_UNDEFINED_ID;
        if (acl_short(e->tag))
            continue;
        kid = e->tag == ACL_USER || e->tag == ACL_GROUP ? acl_id_in(ext2_le32(p + 4), e->tag) : -1;
        if (kid < 0) {
            acl_free(a);
            return -EINVAL;
        }
        e->id = (uint32_t)kid;
    }
    return 0;
}

/** Encode `a` in the xattr form into `out` of `size` bytes, as Linux's
 * `posix_acl_to_xattr`: its bytes, which `size` 0 only asks for, or
 * -ERANGE when they do not fit. */
static int acl_to_xattr(const struct ext2_acl *a, uint8_t *out, uint32_t size)
{
    uint32_t i, need = 4 + a->count * 8;

    if (!size)
        return (int)need;
    if (size < need)
        return -ERANGE;
    LE32_PUT(out, ACL_XATTR_VERSION);
    for (i = 0; i < a->count; i++) {
        const struct ext2_acl_entry *e = &a->e[i];
        uint8_t *p = out + 4 + i * 8;

        LE16_PUT(p, e->tag);
        LE16_PUT(p + 2, e->perm);
        LE32_PUT(p + 4, acl_short(e->tag) ? ACL_UNDEFINED_ID : acl_id_out(e->id, e->tag));
    }
    return (int)need;
}

/**
 * Check the order of `a`, as Linux's `posix_acl_valid`: the owner's entry,
 * the named users', the owning group's, the named groups', the mask, which
 * any named entry needs, and the others', each tag once but the named ones,
 * with no permission bit beyond read, write and execute. 0 or -EINVAL.
 */
static int acl_valid(const struct ext2_acl *a)
{
    uint32_t i, state = ACL_USER_OBJ;
    int needs_mask = 0;

    for (i = 0; i < a->count; i++) {
        uint32_t tag = a->e[i].tag;

        if (a->e[i].perm & ~ACL_RWX)
            return -EINVAL;
        if (tag == ACL_USER_OBJ && state == ACL_USER_OBJ)
            state = ACL_USER;
        else if ((tag == ACL_USER && state == ACL_USER) || (tag == ACL_GROUP && state == ACL_GROUP))
            needs_mask = 1;
        else if (tag == ACL_GROUP_OBJ && state == ACL_USER)
            state = ACL_GROUP;
        else if (tag == ACL_MASK && state == ACL_GROUP)
            state = ACL_OTHER;
        else if (tag == ACL_OTHER && (state == ACL_OTHER || (state == ACL_GROUP && !needs_mask)))
            state = 0;
        else
            return -EINVAL;
    }
    return state == 0 ? 0 : -EINVAL;
}

/**
 * The permission bits `a` gives the owner, the group class and the others,
 * into those of `*mode`, as Linux's `posix_acl_equiv_mode`: 1 when the mode
 * cannot say all `a` says, since it has named entries or a mask, 0 when it
 * can, or -EINVAL for a tag Linux does not know.
 */
static int acl_equiv_mode(const struct ext2_acl *a, uint32_t *mode)
{
    uint32_t i, bits = 0;
    int not_equiv = 0;

    for (i = 0; i < a->count; i++) {
        uint32_t perm = a->e[i].perm & ACL_RWX;

        switch (a->e[i].tag) {
        case ACL_USER_OBJ:
            bits |= perm << 6;
            break;
        case ACL_GROUP_OBJ:
            bits |= perm << 3;
            break;
        case ACL_OTHER:
            bits |= perm;
            break;
        case ACL_MASK:
            bits = (bits & ~0070u) | (perm << 3);
            not_equiv = 1;
            break;
        case ACL_USER:
        case ACL_GROUP:
            not_equiv = 1;
            break;
        default:
            return -EINVAL;
        }
    }
    *mode = (*mode & ~0777u) | bits;
    return not_equiv;
}

/** Whether the entry `i` of `a` grants `want`, limited by the mask that
 * follows it, as Linux's `posix_acl_permission` ends at its `mask` label. */
static int acl_masked(const struct ext2_acl *a, uint32_t i, uint32_t want)
{
    uint32_t j;

    for (j = i + 1; j < a->count; j++)
        if (a->e[j].tag == ACL_MASK)
            return (a->e[i].perm & a->e[j].perm & want) == want ? 0 : -EACCES;
    return (a->e[i].perm & want) == want ? 0 : -EACCES;
}

/**
 * Judge the `want` bits of node `n` by its access ACL `a`, as Linux's
 * `posix_acl_permission`: the owner by its entry, a named user by theirs
 * under the mask, and a member of the owning group or of named groups by
 * the first such entry that grants all of `want`, under the mask, else
 * refused; anyone else by the others' entry. `use_real` judges by the real
 * ids. 0, -EACCES, or -EIO for an ACL without the others' entry or with a
 * tag Linux does not know.
 */
static int acl_permission(const struct ext2_node *n, const struct ext2_acl *a, uint32_t want,
                          int use_real)
{
    process_t *p = current_proc;
    uint32_t i, uid = use_real ? p->ruid : proc_fsuid(p);
    int found = 0;

    want &= ACL_RWX;
    for (i = 0; i < a->count; i++) {
        const struct ext2_acl_entry *e = &a->e[i];

        switch (e->tag) {
        case ACL_USER_OBJ:
            if (n->uid == uid)
                return (e->perm & want) == want ? 0 : -EACCES;
            break;
        case ACL_USER:
            if (e->id == uid)
                return acl_masked(a, i, want);
            break;
        case ACL_GROUP_OBJ:
        case ACL_GROUP:
            if (inode_in_group(e->tag == ACL_GROUP ? e->id : n->gid, use_real)) {
                found = 1;
                if ((e->perm & want) == want)
                    return acl_masked(a, i, want);
            }
            break;
        case ACL_MASK:
            break;
        case ACL_OTHER:
            if (found)
                return -EACCES;
            return (e->perm & want) == want ? 0 : -EACCES;
        default:
            return -EIO;
        }
    }
    return -EIO;
}

/**
 * Fit the access ACL `a` a new inode inherits, and the inode's `*mode`, to
 * each other, as Linux's `posix_acl_create_masq`: the owner's and the
 * others' entries and the mode's bits each keep only what both grant, and
 * the mask's entry, or the owning group's without one, does the same with
 * the group bits. 1 when the ACL says more than the mode, 0 when the mode
 * says it all, or -EIO for an ACL without the entries this needs.
 */
static int acl_create_masq(struct ext2_acl *a, uint32_t *mode)
{
    struct ext2_acl_entry *group_obj = NULL, *mask_obj = NULL;
    uint32_t i, m = *mode;
    int not_equiv = 0;

    for (i = 0; i < a->count; i++) {
        struct ext2_acl_entry *e = &a->e[i];

        switch (e->tag) {
        case ACL_USER_OBJ:
            e->perm &= (m >> 6) | ~ACL_RWX;
            m &= (e->perm << 6) | ~0700u;
            break;
        case ACL_USER:
        case ACL_GROUP:
            not_equiv = 1;
            break;
        case ACL_GROUP_OBJ:
            group_obj = e;
            break;
        case ACL_OTHER:
            e->perm &= m | ~ACL_RWX;
            m &= e->perm | ~0007u;
            break;
        case ACL_MASK:
            mask_obj = e;
            not_equiv = 1;
            break;
        default:
            return -EIO;
        }
    }
    if (!mask_obj && !group_obj)
        return -EIO;
    if (!mask_obj)
        mask_obj = group_obj;
    mask_obj->perm &= (m >> 3) | ~ACL_RWX;
    m &= (mask_obj->perm << 3) | ~0070u;
    *mode = (*mode & ~0777u) | (m & 0777u);
    return not_equiv;
}

/** Give the owner's, the mask's, or the owning group's without a mask, and
 * the others' entries of `a` the bits of `mode`, as Linux's
 * `__posix_acl_chmod` does. 0, or -EIO for an ACL without the entries this
 * needs. */
static int acl_chmod_masq(struct ext2_acl *a, uint32_t mode)
{
    struct ext2_acl_entry *group_obj = NULL, *mask_obj = NULL;
    uint32_t i;

    for (i = 0; i < a->count; i++) {
        struct ext2_acl_entry *e = &a->e[i];

        switch (e->tag) {
        case ACL_USER_OBJ:
            e->perm = (mode & 0700u) >> 6;
            break;
        case ACL_USER:
        case ACL_GROUP:
            break;
        case ACL_GROUP_OBJ:
            group_obj = e;
            break;
        case ACL_MASK:
            mask_obj = e;
            break;
        case ACL_OTHER:
            e->perm = mode & 0007u;
            break;
        default:
            return -EIO;
        }
    }
    if (!mask_obj && !group_obj)
        return -EIO;
    (mask_obj ? mask_obj : group_obj)->perm = (mode & 0070u) >> 3;
    return 0;
}

/** Read the ACL of name index `index` of node `n` into `a`, empty for none,
 * as Linux's `ext2_get_acl`. 0, or the error of reading or decoding it. */
static int acl_get(struct ext2_mount *em, struct ext2_node *n, int index, struct ext2_acl *a)
{
    uint8_t *v = kmalloc(em->block_size);
    int rc;

    a->count = 0;
    a->e = NULL;
    if (!v)
        return -ENOMEM;
    rc = ext2_attr_get(em, n, index, "", v, em->block_size);
    if (rc > 0)
        rc = acl_from_disk(v, (uint32_t)rc, a);
    else if (rc == -ENODATA)
        rc = 0;
    kfree(v);
    return rc;
}

/**
 * Store `a` as the ACL of name index `index` of node `n`, or remove that
 * ACL for an empty `a`, which succeeds where there is none, as Linux's
 * `__ext2_set_acl`. The caller holds `n`'s lock and is admitted. 0,
 * -ERANGE for an ACL larger than a block, or the error of the store.
 */
static int acl_set(struct vfs_mount *mnt, struct ext2_node *n, int index, const struct ext2_acl *a)
{
    struct ext2_mount *em = ext2_em_of(mnt);
    struct vfs_xattr arg;
    uint8_t *v = NULL;
    int rc;

    k_memset(&arg, 0, sizeof(arg));
    arg.name = "";
    arg.op = a->count ? VFS_XATTR_SET : VFS_XATTR_REMOVE;
    if (a->count) {
        arg.size = acl_disk_size(a);
        if (arg.size > em->block_size)
            return -ERANGE;
        v = kmalloc(arg.size);
        if (!v)
            return -ENOMEM;
        acl_to_disk(a, v);
        arg.value = v;
    }
    rc = ext2_attr_set(mnt, n, index, &arg);
    kfree(v);
    return rc == -ENODATA && !a->count ? 0 : rc;
}

/**
 * Store `a` as the access ACL of node `n` and its permission bits in the
 * mode, as Linux's `ext2_set_acl` with `posix_acl_update_mode`: an ACL the
 * mode says all of is removed instead, and the set-group-ID bit is cleared
 * for a caller outside the file's group without CAP_FSETID over it. The
 * caller holds `n`'s lock and is admitted. 0 or -errno.
 */
static int acl_set_access(struct vfs_mount *mnt, struct ext2_node *n, struct ext2_acl *a)
{
    uint32_t mode = n->mode;
    int rc = acl_equiv_mode(a, &mode);

    if (rc < 0)
        return rc;
    if (rc == 0)
        acl_free(a);
    if (current_proc && !inode_in_group(n->gid, 0) && !inode_capable(n->uid, n->gid, CAP_FSETID))
        mode &= ~S_ISGID_;
    rc = acl_set(mnt, n, EXT2_XATTR_ACL_ACCESS, a);
    if (rc)
        return rc;
    EXT2_NODE_SET(n, mode, mode);
    ext2_stamp(n, T_CTIME);
    return ext2_inode_store(ext2_em_of(mnt), n);
}

/** Whether the strings `a` and `b` are equal. */
static int acl_name_is(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

int ext2_acl_index(const char *name)
{
    if (acl_name_is(name, "system.posix_acl_access"))
        return EXT2_XATTR_ACL_ACCESS;
    if (acl_name_is(name, "system.posix_acl_default"))
        return EXT2_XATTR_ACL_DEFAULT;
    return 0;
}

/**
 * The write of an ACL by setxattr or removexattr, as Linux's `do_set_acl`
 * and `set_posix_acl`: a value is decoded first; then a mount without ACLs
 * and a symbolic link are -EOPNOTSUPP, a default ACL of anything but a
 * directory is -EACCES, or nothing for a removal, a caller who neither owns
 * the file nor holds CAP_FOWNER over it -EPERM, and an ACL out of order
 * -EINVAL. An empty value removes the ACL, and the flags are not read.
 */
static int acl_xattr_set(struct vfs_mount *mnt, struct ext2_node *n, int index,
                         const struct vfs_xattr *arg)
{
    struct ext2_acl a;
    int rc = 0;

    a.count = 0;
    a.e = NULL;
    if (arg->op == VFS_XATTR_SET && arg->size)
        rc = acl_from_xattr(arg->value, arg->size, &a);
    if (!rc && !mnt->sb_posixacl)
        rc = -EOPNOTSUPP;
    else if (!rc && (n->mode & S_IFMT_) == S_IFLNK_)
        rc = -EOPNOTSUPP;
    else if (!rc && index == EXT2_XATTR_ACL_DEFAULT && (n->mode & S_IFMT_) != S_IFDIR_)
        rc = a.count ? -EACCES : 1;
    else if (!rc && !inode_owner_or_capable(n->uid))
        rc = -EPERM;
    else if (!rc && a.count)
        rc = acl_valid(&a);
    if (!rc)
        rc = index == EXT2_XATTR_ACL_ACCESS && a.count ? acl_set_access(mnt, n, &a)
                                                       : acl_set(mnt, n, index, &a);
    acl_free(&a);
    return rc == 1 ? 0 : rc;
}

int ext2_acl_xattr(struct vfs_mount *mnt, struct ext2_node *n, int index, struct vfs_xattr *arg)
{
    struct ext2_acl a;
    int rc;

    if (arg->op >= VFS_XATTR_SET)
        return acl_xattr_set(mnt, n, index, arg);
    if (!mnt->sb_posixacl || (n->mode & S_IFMT_) == S_IFLNK_)
        return -EOPNOTSUPP;
    rc = acl_get(ext2_em_of(mnt), n, index, &a);
    if (!rc)
        rc = a.count ? acl_to_xattr(&a, arg->value, arg->size) : -ENODATA;
    acl_free(&a);
    return rc;
}

int ext2_check_acl(struct vfs_mount *mnt, void *node, int want, int use_real)
{
    struct ext2_node *n = node;
    struct ext2_acl a;
    int rc;

    if (!n->file_acl || n->acl_absent)
        return -EAGAIN;
    rc = acl_get(ext2_em_of(mnt), n, EXT2_XATTR_ACL_ACCESS, &a);
    if (!rc)
        rc = a.count ? acl_permission(n, &a, (uint32_t)want, use_real) : -EAGAIN;
    acl_free(&a);
    return rc;
}

int ext2_acl_inherit(struct ext2_mount *em, struct ext2_node *dir, uint32_t *mode,
                     struct ext2_acl_new *out)
{
    struct ext2_acl *p = &out->dflt, *c = &out->access;
    int rc;

    k_memset(out, 0, sizeof(*out));
    if (!em->mnt->sb_posixacl || (*mode & S_IFMT_) == S_IFLNK_)
        return 0;
    rc = acl_get(em, dir, EXT2_XATTR_ACL_DEFAULT, p);
    if (rc)
        return rc;
    if (!p->count) {
        *mode &= ~(current_proc ? current_proc->fs->umask : 0u);
        return 0;
    }
    rc = acl_alloc(c, p->count);
    if (!rc) {
        k_memcpy(c->e, p->e, p->count * sizeof(*p->e));
        rc = acl_create_masq(c, mode);
    }
    if (rc <= 0)
        acl_free(c);
    if ((*mode & S_IFMT_) != S_IFDIR_)
        acl_free(p);
    if (rc < 0) {
        acl_free(p);
        return rc;
    }
    return 0;
}

int ext2_acl_init(struct vfs_mount *mnt, struct ext2_node *n, struct ext2_acl_new *acls)
{
    int rc = 0;

    if (acls->dflt.count)
        rc = acl_set(mnt, n, EXT2_XATTR_ACL_DEFAULT, &acls->dflt);
    if (!rc && acls->access.count)
        rc = acl_set(mnt, n, EXT2_XATTR_ACL_ACCESS, &acls->access);
    ext2_acl_new_free(acls);
    return rc;
}

void ext2_acl_new_free(struct ext2_acl_new *acls)
{
    acl_free(&acls->access);
    acl_free(&acls->dflt);
}

int ext2_acl_chmod(struct vfs_mount *mnt, struct ext2_node *n)
{
    struct ext2_acl a;
    int rc;

    if (!mnt->sb_posixacl || (n->mode & S_IFMT_) == S_IFLNK_)
        return 0;
    rc = acl_get(ext2_em_of(mnt), n, EXT2_XATTR_ACL_ACCESS, &a);
    if (!rc && a.count)
        rc = acl_chmod_masq(&a, n->mode);
    if (!rc && a.count)
        rc = acl_set_access(mnt, n, &a);
    acl_free(&a);
    return rc;
}
