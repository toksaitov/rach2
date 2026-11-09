/* The entries root makes under /dev (fs/devtree.h) for models that reach no
 * /dev: there are none, and none can be made. */
#include "fs/devtree.h"
#include "lib/kerrno.h"

int devtree_child(int parent, const char *name, uint32_t len)
{
    (void)parent;
    (void)name;
    (void)len;
    return 0;
}

int devtree_resolve(const char *rel, uint32_t len)
{
    (void)rel;
    (void)len;
    return -ENOENT;
}

int devtree_is_dir(int id)
{
    (void)id;
    return -ENOENT;
}

int devtree_attr(int id, struct devtree_attr *a)
{
    (void)id;
    (void)a;
    return -ENOENT;
}

int devtree_readlink(int id, char *buf, uint32_t cap)
{
    (void)id;
    (void)buf;
    (void)cap;
    return -ENOENT;
}

void devtree_path(int id, char *out, int cap)
{
    (void)id;
    if (cap > 1) {
        out[0] = '/';
        out[1] = '\0';
    }
}

int devtree_dirent(int parent, int n, const char **name, uint32_t *ino, uint8_t *dtype)
{
    (void)parent;
    (void)n;
    (void)name;
    (void)ino;
    (void)dtype;
    return 0;
}

int devtree_make(int parent, const char *name, uint32_t mode, const char *target, uint32_t uid,
                 uint32_t gid)
{
    (void)parent;
    (void)name;
    (void)mode;
    (void)target;
    (void)uid;
    (void)gid;
    return -ENOENT;
}

int devtree_remove(int parent, const char *name, int dir)
{
    (void)parent;
    (void)name;
    (void)dir;
    return -ENOENT;
}

int devtree_rename(int from, const char *oname, int to, const char *nname, int noreplace)
{
    (void)from;
    (void)oname;
    (void)to;
    (void)nname;
    (void)noreplace;
    return -ENOENT;
}

int devtree_setattr(int id, uint32_t mode, uint32_t uid, uint32_t gid)
{
    (void)id;
    (void)mode;
    (void)uid;
    (void)gid;
    return -ENOENT;
}
