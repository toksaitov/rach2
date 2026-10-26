/*
 * krffs_stat.c: the inode number the KRFFS root gives to the old and the
 * wide stat calls, through the stat functions of rach2/sys/syscall_fs.c and
 * vfs.c's kfs_getattr, over krffs_hosttest.c's core and image.
 *
 * The rules of mk/host-fixtures.mk extract the functions unchanged with
 * extract.c: krffs_stat_types.inc has the stat structures of
 * arch/i686/uabi.h, fs/vfs.h and syscall_fs.c, and krffs_stat_funcs.inc the
 * stat functions of syscall_fs.c and kfs_getattr. This file supplies the user
 * copies, a resolver backed by the KRFFS walk, and the root mount the old
 * stat asks for. It runs krffs_hosttest.c's rows first, and passes only when
 * they pass.
 */
#define main core_main
#include "../krffs_hosttest.c"
#undef main

#include "krffs_stat_types.inc"

#define __user
#define EFAULT            14
#define S_IFMT            0170000
#define S_IFDIR           0040000
#define ENOTDIR           20
#define S_IFREG           0100000
#define VFS_S_IFDIR       S_IFDIR
#define VFS_S_IFREG       S_IFREG
#define VFS_WALK_NOFOLLOW 1
#define VFS_WALK_LOCK     2
#define PATH_END_SLASH    1
#define PATH_END_DOT      2
#define PATH_END_DOTDOT   3

/* The kernel's interfaces the stat functions call, reduced to one root mount
 * whose walk is krffs_resolve. */
static int current_proc = 1;
struct vfs_type {
    int unused;
};
static const struct vfs_type vfs_krffs_type;
struct vfs_mount {
    const struct vfs_type *type;
};
static struct vfs_mount root_mount = {&vfs_krffs_type};

static struct vfs_mount *vfs_root_mount(void)
{
    return &root_mount;
}
struct vfs_ref {
    struct vfs_mount *mnt;
    void *node;
};

static void vfs_walk_unlock(struct vfs_mount *m)
{
    (void)m;
}
struct vpath {
    int unused;
};
#define AT_FDCWD (-100)
static char path_page[4096];

static int path_from_user(int dirfd, const char *in, char **out)
{
    unsigned n = k_strlen(in) + 1;
    (void)dirfd;
    if (n > sizeof(path_page)) {
        return -1;
    }
    k_memcpy(path_page, in, n);
    *out = path_page;
    return 0;
}
static int path_names_dir(const char *raw, int len);

static int path_from_user_dir(int dirfd, const char *in, char **out, int *dir)
{
    if (dir) {
        *dir = path_names_dir(in, (int)k_strlen(in));
    }
    return path_from_user(dirfd, in, out);
}

static void path_buf_put(char *buf)
{
    (void)buf;
}

static int copy_to_user(void *out, const void *in, unsigned n)
{
    k_memcpy(out, in, n);
    return 0;
}

static int vfs_resolve(int a, const char *p, int f, struct vfs_ref *r, int b, int c)
{
    (void)a;
    (void)f;
    (void)b;
    (void)c;
    r->mnt = 0;
    r->node = krffs_resolve(p);
    return r->node ? 0 : -1;
}

static const struct vpath *vfs_synth_vpath(struct vfs_mount *m, void *n)
{
    (void)m;
    (void)n;
    return 0;
}

static int mnt_is_root(struct vfs_mount *m)
{
    (void)m;
    return 1;
}

static void vfs_ref_release(struct vfs_ref *r)
{
    (void)r;
}

static int synth_stat(const struct vpath *p, struct kstat *s, int f)
{
    (void)p;
    (void)s;
    (void)f;
    return -1;
}

static int vfs_fill_stat64(struct kstat *s, const struct vfs_ref *r)
{
    (void)s;
    (void)r;
    return -1;
}

static void stat64_boot_times(struct kstat *s)
{
    (void)s;
}

static int names_tree_root(const char *p)
{
    return p[0] == '/' && p[1] == '\0';
}

static uint32_t uid_seen(uint32_t kid)
{
    return kid;
}

static uint32_t gid_seen(uint32_t kid)
{
    return kid;
}

#include "krffs_stat_funcs.inc"

int main(void)
{
    const char *paths[] = {"/", "/tmp/.."};
    unsigned i;
    if (core_main()) {
        return 1;
    }
    make_image(image, IMAGE_SIZE);
    ((krffs_node_t *)image)->mode = 0755;
    check(init_krffs(image, IMAGE_SIZE) == 0 && krffs_mkdir("/tmp") == 0, "root stat fixture");
    for (i = 0; i < 2; i++) {
        struct linux_stat old;
        struct user_stat wide;
        struct kstat node;
        struct vfs_stat attr;
        check(do_old_stat(paths[i], &old, 1) == 0 && old.st_ino == 1 &&
                  (old.st_mode & 0170000) == S_IFDIR,
              "root old stat inode is 1");
        check(do_stat64(paths[i], &wide, 1) == 0 && wide.st_ino == 1 && wide.__st_ino == 1,
              "root stat64 inode is 1");
        krffs_fill_stat64(&node, krffs_resolve(paths[i]));
        check(node.st_ino == 1, "root node stat64 inode is 1");
        check(kfs_getattr(0, krffs_resolve(paths[i]), &attr) == 0 && attr.ino == 1,
              "root VFS inode is 1");
    }
    if (fails) {
        return 1;
    }
    printf("krffs-stat: root stat paths pass\n");
    return 0;
}
