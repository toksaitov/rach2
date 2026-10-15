/*
 * krffs_fuse_operations.c: the FUSE handlers of krffs_fuse_operations.h.
 */

#include "krffs_fuse_operations.h"

#include "krffs_allocator.h"
#include "krffs_file_system.h"
#include "krffs_node.h"
#include "krffs_platform.h"
#include "krffs_utilities.h"

#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

extern struct krffs_file_system *KRFFS_File_System;

/**
 * Fills `stat_buffer` from `node` with `file_type` as the type and
 * `hard_links_count` links. The mode takes the node's 07777 bits, or at the
 * root only the 0777 bits, since the dirty flag there is the set-user-ID bit.
 */
static inline void fill_stat_buffer_for_node(struct PLATFORM_STAT *stat_buffer,
                                             struct krffs_node *node, uint16_t file_type,
                                             uint16_t hard_links_count)
{
    stat_buffer->st_nlink = hard_links_count;
    stat_buffer->st_size = node->data_size;
    stat_buffer->st_mode =
        file_type |
        (node->mode & (KRFFS_File_System && node == KRFFS_File_System->node ? 0777u : 07777u));
    stat_buffer->st_uid = node->uid;
    stat_buffer->st_gid = node->gid;
#if defined(APPLE) && (!defined(_POSIX_C_SOURCE) || defined(_DARWIN_C_SOURCE))
    stat_buffer->st_atimespec = krffs_get_timespec_from_node_time(&node->access_time);
    stat_buffer->st_mtimespec = krffs_get_timespec_from_node_time(&node->modification_time);
    stat_buffer->st_ctimespec = krffs_get_timespec_from_node_time(&node->attribute_change_time);
#elif defined(WINDOWS)
    stat_buffer->st_atim = krffs_get_timespec_from_node_time(&node->access_time);
    stat_buffer->st_mtim = krffs_get_timespec_from_node_time(&node->modification_time);
    stat_buffer->st_ctim = krffs_get_timespec_from_node_time(&node->attribute_change_time);
#else
    stat_buffer->st_atime = krffs_get_time_from_node_time(&node->access_time);
    stat_buffer->st_mtime = krffs_get_time_from_node_time(&node->modification_time);
    stat_buffer->st_ctime = krffs_get_time_from_node_time(&node->attribute_change_time);
#endif
}

/**
 * Returns the node at `path`, or NULL, with a line on stderr, when there is
 * no file system or no node there.
 */
static struct krffs_node *find_node(const char *path)
{
    struct krffs_node *node = NULL;

    if (!KRFFS_File_System) {
        fprintf(stderr, "krffs: no file system\n");
    } else if (!(node = krffs_find_node_by_path(KRFFS_File_System, path, strlen(path)))) {
        fprintf(stderr, "krffs: not found: %s\n", path);
    }

    return node;
}

/**
 * Finds the directory that is to hold the last component of `path`, sets
 * `*directory` to it and `*name` to that component, a pointer into `path`.
 * Returns 0, -ENOENT when there is no file system or no node at the
 * directory's path, -ENOTDIR when that node is not a directory, -EINVAL for
 * an empty last component, and -ENAMETOOLONG for one of more than 255
 * bytes.
 */
static int find_parent(const char *path, struct krffs_node **directory, const char **name)
{
    const char *slash = strrchr(path, '/');
    const char *problem = NULL;
    int result = 0;

    *directory = NULL;
    *name = slash ? slash + 1 : path;

    if (!KRFFS_File_System || !slash) {
        result = -ENOENT;
        problem = "no file system or no directory";
    } else if (!(*directory =
                     krffs_find_node_by_path(KRFFS_File_System, path, (size_t)(slash - path)))) {
        result = -ENOENT;
        problem = "no directory";
    } else if (!krffs_is_directory(KRFFS_File_System, *directory)) {
        result = -ENOTDIR;
        problem = "not a directory";
    } else if (**name == '\0') {
        result = -EINVAL;
        problem = "empty name";
    } else if (strlen(*name) >= KRFFS_FILE_NAME_BUFFER_SIZE) {
        result = -ENAMETOOLONG;
        problem = "name too long";
    }
    if (problem) {
        fprintf(stderr, "krffs: %s: %s\n", problem, path);
    }

    return result;
}

/**
 * Finds the directory and the name of `path` as `find_parent` does, and sets
 * `*node` to the node of that name in the directory, or to NULL when there is
 * none. Returns what `find_parent` returns.
 */
static int find_entry(const char *path, struct krffs_node **directory, const char **name,
                      struct krffs_node **node)
{
    int result = find_parent(path, directory, name);

    *node = NULL;
    if (result == 0) {
        *node = krffs_find_child(KRFFS_File_System,
                                 krffs_get_directory_id(KRFFS_File_System, *directory), *name);
    }

    return result;
}

/** Sets the modification and change times of `directory` to `time`. */
static void touch_directory(struct krffs_node *directory, struct krffs_node_time time)
{
    directory->modification_time = time;
    directory->attribute_change_time = time;

    krffs_sync_mapping(directory, sizeof(*directory));
}

/**
 * Counts the nodes with a name in the directory whose id is `directory_id`,
 * the directories alone when `directories_only` is set.
 */
static size_t count_children(uint64_t directory_id, int directories_only)
{
    size_t count = 0;

    struct krffs_node *node = KRFFS_File_System->node;
    do {
        if (node->type == KRFFS_Reserved_Node && node->parent_id == directory_id &&
            node->name[0] != '\0' &&
            (!directories_only || krffs_is_directory(KRFFS_File_System, node))) {
            ++count;
        }
    } while ((node = krffs_get_next_node(KRFFS_File_System, node)) != KRFFS_File_System->node);

    return count;
}

/**
 * Gives `node` the user and group of the request FUSE is serving, or, when
 * FUSE reports no request, the driver's effective user and group, which
 * `mkfs.krffs` gives the root (0:0 on Windows).
 */
static void set_owner(struct krffs_node *node)
{
    struct fuse_context *context = fuse_get_context();

    if (context) {
        node->uid = context->uid;
        node->gid = context->gid;
    } else {
#ifdef WINDOWS
        node->uid = 0;
        node->gid = 0;
#else
        node->uid = geteuid();
        node->gid = getegid();
#endif
    }
}

/**
 * Reserves a node named `name` in `directory` with `mode`, the caller's
 * owner, and the current time as its three times and as the directory's
 * modification and change times. Its id is the djb2 hash of the name, as the
 * kernel gives a file, or the hash mixed with the directory's id for a
 * directory. Returns the node, or NULL when no free node has room for a
 * header.
 */
static struct krffs_node *make_node(struct krffs_node *directory, const char *name, uint16_t mode)
{
    uint64_t directory_id = krffs_get_directory_id(KRFFS_File_System, directory);
    struct krffs_node *node = krffs_allocate_reserved_node(KRFFS_File_System, 0, NULL);

    if (node) {
        krffs_set_reserved_node_name(node, name);

        node->parent_id = directory_id;
        node->mode = mode;
        if ((mode & KRFFS_MODE_IFMT) == KRFFS_MODE_IFDIR) {
            node->id = krffs_mix_id(directory_id, name);
        }

        set_owner(node);

        struct krffs_node_time current_node_time = krffs_get_current_node_time();

        node->access_time = current_node_time;
        node->modification_time = current_node_time;
        node->attribute_change_time = current_node_time;

        krffs_sync_mapping(node, sizeof(*node));

        touch_directory(directory, current_node_time);
    }

    return node;
}

/**
 * Reports whether a directory has the id that a directory named `name` in
 * `directory` would get, which the kernel's `krffs_id_taken` refuses.
 */
static int directory_id_taken(struct krffs_node *directory, const char *name)
{
    uint64_t id = krffs_mix_id(krffs_get_directory_id(KRFFS_File_System, directory), name);

    return krffs_find_directory_by_id(KRFFS_File_System, id) != NULL;
}

int krffs_getattr(const char *path, struct PLATFORM_STAT *stat_buffer)
{
    int result = 0;

    memset(stat_buffer, 0, sizeof(*stat_buffer));

    struct krffs_node *node = find_node(path);

    if (!node) {
        result = -ENOENT;
    } else if (krffs_is_directory(KRFFS_File_System, node)) {
        size_t subdirectories = count_children(krffs_get_directory_id(KRFFS_File_System, node), 1);

        fill_stat_buffer_for_node(stat_buffer, node, S_IFDIR, (uint16_t)(2 + subdirectories));
    } else {
        fill_stat_buffer_for_node(stat_buffer, node, S_IFREG, 1);
    }

    return result;
}

int krffs_readdir(const char *path, void *buffer, fuse_fill_dir_t filler, PLATFORM_OFF_T offset,
                  struct fuse_file_info *file_info)
{
    (void)offset;
    (void)file_info;

    int result = 0;

    struct krffs_node *directory = find_node(path);

    if (!directory) {
        result = -ENOENT;
    } else if (!krffs_is_directory(KRFFS_File_System, directory)) {
        result = -ENOTDIR;
    } else {
        uint64_t directory_id = krffs_get_directory_id(KRFFS_File_System, directory);

        filler(buffer, ".", NULL, 0);
        filler(buffer, "..", NULL, 0);

        struct krffs_node *node = KRFFS_File_System->node;
        do {
            if (node->type == KRFFS_Reserved_Node && node->parent_id == directory_id &&
                node->name[0] != '\0') {
                filler(buffer, (const char *)node->name, NULL, 0);
            }
        } while ((node = krffs_get_next_node(KRFFS_File_System, node)) != KRFFS_File_System->node);
    }

    return result;
}

int krffs_open(const char *path, struct fuse_file_info *file_info)
{
    (void)file_info;

    return find_node(path) ? 0 : -ENOENT;
}

int krffs_read(const char *path, char *buffer, size_t size, PLATFORM_OFF_T offset,
               struct fuse_file_info *file_info)
{
    (void)file_info;

    int result = 0;

    struct krffs_node *node = find_node(path);

    if (!node) {
        result = -ENOENT;
    } else if (krffs_is_directory(KRFFS_File_System, node)) {
        result = -EISDIR;
    } else {
        void *base = node + 1;
        uint64_t data_size = node->data_size;

        if ((uint64_t)offset < data_size) {
            if ((uint64_t)offset + size > data_size) {
                size = data_size - offset;
            }

            memcpy(buffer, ((char *)base) + offset, size);

            node->access_time = krffs_get_current_node_time();
        } else {
            size = 0;
        }

        result = (int)size;
    }

    return result;
}

int krffs_mknod(const char *path, mode_t mode, dev_t device)
{
    (void)device;

    struct krffs_node *directory;
    const char *name;
    struct krffs_node *node;

    int result = find_entry(path, &directory, &name, &node);

    if (result == 0 && node) {
        fprintf(stderr, "krffs: exists: %s\n", path);

        result = -EEXIST;
    } else if (result == 0 && !make_node(directory, name, (uint16_t)mode)) {
        fprintf(stderr, "krffs: no space: %s\n", path);

        result = -ENOSPC;
    }

    return result;
}

int krffs_create(const char *path, mode_t mode, struct fuse_file_info *file_info)
{
    (void)file_info;

    struct krffs_node *directory;
    const char *name;
    struct krffs_node *node;

    int result = find_entry(path, &directory, &name, &node);

    if (result == 0 && node && krffs_is_directory(KRFFS_File_System, node)) {
        result = -EISDIR;
    } else if (result == 0 && !node && !make_node(directory, name, (uint16_t)mode)) {
        fprintf(stderr, "krffs: no space: %s\n", path);

        result = -ENOSPC;
    }

    return result;
}

int krffs_mkdir(const char *path, mode_t mode)
{
    struct krffs_node *directory;
    const char *name;
    struct krffs_node *node;

    int result = find_entry(path, &directory, &name, &node);

    if (result == 0 && node) {
        fprintf(stderr, "krffs: exists: %s\n", path);

        result = -EEXIST;
    } else if (result == 0 && directory_id_taken(directory, name)) {
        fprintf(stderr, "krffs: directory id taken: %s\n", path);

        result = -EINVAL;
    } else if (result == 0 &&
               !make_node(directory, name, (uint16_t)(KRFFS_MODE_IFDIR | (mode & 07777u)))) {
        fprintf(stderr, "krffs: no space: %s\n", path);

        result = -ENOSPC;
    }

    return result;
}

/**
 * Copies `size` bytes from `buffer` into the data of `node` at `offset`,
 * syncs them, and sets the modification time. The node must already have
 * room for them. When `offset` is past `old_data_size`, the data size before
 * the caller grew the node, the gap is cleared and synced first, since POSIX
 * has a gap read as zero bytes.
 */
static void krffs_write_into_node(struct krffs_node *node, const char *buffer, size_t size,
                                  PLATFORM_OFF_T offset, uint64_t old_data_size)
{
    void *base = node + 1;
    void *destination = ((char *)base) + offset;

    if ((uint64_t)offset > old_data_size) {
        void *gap = ((char *)base) + old_data_size;
        size_t gap_size = (size_t)((uint64_t)offset - old_data_size);

        memset(gap, 0, gap_size);
        krffs_sync_mapping(gap, gap_size);
    }
    memcpy(destination, buffer, size);

    krffs_sync_mapping(destination, size);

    node->modification_time = krffs_get_current_node_time();
}

int krffs_write(const char *path, const char *buffer, size_t size, PLATFORM_OFF_T offset,
                struct fuse_file_info *file_info)
{
    (void)file_info;

    int result = 0;

    struct krffs_node *node = find_node(path);

    if (!node) {
        result = -ENOENT;
    } else if (krffs_is_directory(KRFFS_File_System, node)) {
        result = -EISDIR;
    } else {
        uint64_t available_data_size = node->size - sizeof(*node);
        uint64_t old_data_size = node->data_size;

        uint64_t new_size = offset + size;
        if (new_size <= available_data_size) {
            if (new_size > node->data_size) {
                node->data_size = new_size;
            }

            krffs_sync_mapping(node, sizeof(*node));

            krffs_write_into_node(node, buffer, size, offset, old_data_size);

            result = (int)size;
        } else if (!(node = krffs_resize_reserved_node(KRFFS_File_System, node, new_size))) {
            fprintf(stderr, "krffs: no space: %s\n", path);

            result = -ENOSPC;
        } else {
            krffs_write_into_node(node, buffer, size, offset, old_data_size);

            result = (int)size;
        }
    }

    return result;
}

int krffs_fsync(const char *path, int is_data_sync, struct fuse_file_info *file_info)
{
    (void)is_data_sync;
    (void)file_info;

    return find_node(path) ? 0 : -ENOENT;
}

int krffs_truncate(const char *path, PLATFORM_OFF_T new_size)
{
    int result = 0;

    struct krffs_node *node = find_node(path);
    uint64_t data_size = node ? node->data_size : 0;

    if (!node) {
        result = -ENOENT;
    } else if (krffs_is_directory(KRFFS_File_System, node)) {
        result = -EISDIR;
    } else if (!(node = krffs_resize_reserved_node(KRFFS_File_System, node, new_size))) {
        fprintf(stderr, "krffs: no space: %s\n", path);

        result = -ENOSPC;
    } else {
        if ((uint64_t)new_size > data_size) {
            void *base = node + 1;

            void *destination = ((char *)base) + data_size;
            uint64_t size = (uint64_t)new_size - data_size;

            memset(destination, 0, size);

            krffs_sync_mapping(destination, size);
        }

        node->modification_time = krffs_get_current_node_time();
    }

    return result;
}

/**
 * Removes the target of a rename, the node `node` at `path`, and returns 0,
 * or returns -EISDIR, with nothing removed, when it is a directory, as the
 * kernel's rename refuses to replace one.
 */
static int remove_rename_target(struct krffs_node *node, const char *path)
{
    int result = 0;

    if (krffs_is_directory(KRFFS_File_System, node)) {
        fprintf(stderr, "krffs: is a directory: %s\n", path);

        result = -EISDIR;
    } else {
        krffs_remove_reserved_node(KRFFS_File_System, node);
    }

    return result;
}

int krffs_rename(const char *path, const char *new_path)
{
    struct krffs_node *directory;
    struct krffs_node *new_directory;
    const char *name;
    const char *new_name;
    struct krffs_node *node;
    struct krffs_node *target = NULL;

    int result = find_entry(path, &directory, &name, &node);

    if (result == 0 && !node) {
        fprintf(stderr, "krffs: not found: %s\n", path);

        result = -ENOENT;
    } else if (result == 0 && krffs_is_directory(KRFFS_File_System, node)) {
        fprintf(stderr, "krffs: cannot rename a directory: %s\n", path);

        result = -EPERM;
    }
    if (result == 0) {
        result = find_entry(new_path, &new_directory, &new_name, &target);
    }
    if (result == 0 && target && target != node) {
        result = remove_rename_target(target, new_path);
    }
    if (result == 0 && target != node) {
        struct krffs_node_time current_node_time = krffs_get_current_node_time();

        krffs_set_reserved_node_name(node, new_name);
        node->parent_id = krffs_get_directory_id(KRFFS_File_System, new_directory);
        node->attribute_change_time = current_node_time;

        krffs_sync_mapping(node, sizeof(*node));

        touch_directory(directory, current_node_time);
        touch_directory(new_directory, current_node_time);
    }

    return result;
}

int krffs_unlink(const char *path)
{
    struct krffs_node *directory;
    const char *name;
    struct krffs_node *node;

    int result = find_entry(path, &directory, &name, &node);

    if (result == 0 && !node) {
        fprintf(stderr, "krffs: not found: %s\n", path);

        result = -ENOENT;
    } else if (result == 0 && krffs_is_directory(KRFFS_File_System, node)) {
        fprintf(stderr, "krffs: is a directory: %s\n", path);

        result = -EISDIR;
    } else if (result == 0) {
        krffs_remove_reserved_node(KRFFS_File_System, node);

        touch_directory(directory, krffs_get_current_node_time());
    }

    return result;
}

int krffs_rmdir(const char *path)
{
    struct krffs_node *directory;
    const char *name;
    struct krffs_node *node;

    int result = find_entry(path, &directory, &name, &node);

    if (result == 0 && !node) {
        fprintf(stderr, "krffs: not found: %s\n", path);

        result = -ENOENT;
    } else if (result == 0 && !krffs_is_directory(KRFFS_File_System, node)) {
        fprintf(stderr, "krffs: not a directory: %s\n", path);

        result = -ENOTDIR;
    } else if (result == 0 && count_children(node->id, 0) != 0) {
        fprintf(stderr, "krffs: directory not empty: %s\n", path);

        result = -ENOTEMPTY;
    } else if (result == 0) {
        krffs_remove_reserved_node(KRFFS_File_System, node);

        touch_directory(directory, krffs_get_current_node_time());
    }

    return result;
}

int krffs_utimens(const char *path, const struct timespec time[2])
{
    int result = 0;

    struct krffs_node *node = find_node(path);

    if (!node) {
        result = -ENOENT;
    } else {
        struct krffs_node_time current_node_time = krffs_get_current_node_time();

        if (!time || time[0].tv_nsec == UTIME_NOW) {
            node->access_time = current_node_time;
        } else if (time[0].tv_nsec != UTIME_OMIT) {
            node->access_time.seconds = time[0].tv_sec;
            node->access_time.nanoseconds = time[0].tv_nsec;
        }
        if (!time || time[1].tv_nsec == UTIME_NOW) {
            node->modification_time = current_node_time;
        } else if (time[1].tv_nsec != UTIME_OMIT) {
            node->modification_time.seconds = time[1].tv_sec;
            node->modification_time.nanoseconds = time[1].tv_nsec;
        }
        if (!time || time[0].tv_nsec != UTIME_OMIT || time[1].tv_nsec != UTIME_OMIT) {
            node->attribute_change_time = current_node_time;
        }

        krffs_sync_mapping(node, sizeof(*node));
    }

    return result;
}

int krffs_chmod(const char *path, mode_t mode)
{
    int result = 0;

    struct krffs_node *node = find_node(path);

    if (!node) {
        result = -ENOENT;
    } else {
        uint16_t writable = node == KRFFS_File_System->node ? 0777u : 07777u;

        node->mode = (uint16_t)((node->mode & (uint16_t)~writable) | (mode & writable));
        node->attribute_change_time = krffs_get_current_node_time();

        krffs_sync_mapping(node, sizeof(*node));
    }

    return result;
}

int krffs_chown(const char *path, uid_t uid, gid_t gid)
{
    int result = 0;

    struct krffs_node *node = find_node(path);

    if (!node) {
        result = -ENOENT;
    } else {
        if (uid != (uid_t)-1) {
            node->uid = uid;
        }
        if (gid != (gid_t)-1) {
            node->gid = gid;
        }
        node->attribute_change_time = krffs_get_current_node_time();

        krffs_sync_mapping(node, sizeof(*node));
    }

    return result;
}

int krffs_statfs(const char *path, struct statvfs *statvfs_buffer)
{
    int result = 0;

    memset(statvfs_buffer, 0, sizeof(*statvfs_buffer));

    if (!KRFFS_File_System) {
        fprintf(stderr, "No file system to work with.\n");

        result = -ENOENT;
    } else {
        if (!path) {
            result = -EFAULT;
        } else {
            static const unsigned long StatFS_Block_Size = 1024;

            uint64_t total_space = KRFFS_File_System->size;
            uint64_t total_free_space = krffs_calculate_total_free_space(KRFFS_File_System);
            size_t reserved_node_count = krffs_count_reserved_nodes(KRFFS_File_System);

            statvfs_buffer->f_bsize = StatFS_Block_Size;
            statvfs_buffer->f_frsize = StatFS_Block_Size;
            statvfs_buffer->f_blocks = (fsblkcnt_t)((total_space - 1) / StatFS_Block_Size + 1);
            statvfs_buffer->f_bfree = (fsblkcnt_t)(total_free_space / StatFS_Block_Size);
            statvfs_buffer->f_bavail = statvfs_buffer->f_bfree;
            statvfs_buffer->f_files = (fsfilcnt_t)reserved_node_count;
            statvfs_buffer->f_ffree = 0;
            statvfs_buffer->f_favail = statvfs_buffer->f_ffree;
            statvfs_buffer->f_namemax = KRFFS_FILE_NAME_BUFFER_SIZE - 1;
        }
    }

    return result;
}

const struct fuse_operations krffs_operations = {.getattr = krffs_getattr,
                                                 .readdir = krffs_readdir,
                                                 .open = krffs_open,
                                                 .read = krffs_read,
                                                 .mknod = krffs_mknod,
                                                 .create = krffs_create,
                                                 .mkdir = krffs_mkdir,
                                                 .write = krffs_write,
                                                 .fsync = krffs_fsync,
                                                 .truncate = krffs_truncate,
                                                 .unlink = krffs_unlink,
                                                 .rmdir = krffs_rmdir,
                                                 .rename = krffs_rename,
                                                 .utimens = krffs_utimens,
                                                 .chmod = krffs_chmod,
                                                 .chown = krffs_chown,
                                                 .statfs = krffs_statfs};
