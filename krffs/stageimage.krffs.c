/*
 * stageimage.krffs.c: stageimage.krffs, which writes the directories and
 * files a manifest lists into a KRFFS image, or into a host directory for the
 * ext2 builder.
 */

#include <dirent.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "krffs_allocator.h"
#include "krffs_file_system.h"
#include "krffs_node.h"
#include "krffs_platform.h"
#include "krffs_utilities.h"

#ifdef WINDOWS
#include <io.h>
#else
#include <unistd.h>
#endif

/** Returns whether the mode of `node` makes it a directory. */
static bool is_dir_node(const struct krffs_node *node)
{
    return (node->mode & KRFFS_MODE_IFMT) == KRFFS_MODE_IFDIR;
}

/**
 * Returns whether `path` is a clean absolute guest path: `/`, or components
 * each after a `/`, with no empty one, no `.` or `..`, and no `/` at the end.
 * A manifest names each path as it is, so any other path is an error in it;
 * a doubled slash in particular would take a path under `/dev` past
 * `is_synthetic_path`.
 */
static bool is_valid_guest_path(const char *path)
{
    size_t len = strlen(path);

    if (path[0] != '/') {
        return false;
    }
    if (strcmp(path, "/") == 0) {
        return true;
    }
    if (path[len - 1] == '/' || strstr(path, "//") != NULL || strstr(path, "/./") != NULL ||
        strstr(path, "/../") != NULL) {
        return false;
    }
    if (len >= 2 && strcmp(path + len - 2, "/.") == 0) {
        return false;
    }
    if (len >= 3 && strcmp(path + len - 3, "/..") == 0) {
        return false;
    }

    return true;
}

/**
 * Returns the host path a manifest's `path` is read from.
 *
 * A manifest names its host files by their i686 paths, build/... and
 * vendor/musl/install/..., so one manifest describes the image of either
 * architecture. An x86-64 build (the root Makefile, mk/arch.sh) sets
 * RACH2_PATH_MAP to blank-separated `FROM=TO` pairs, and the first FROM that
 * begins `path` is replaced by its TO: build/utils/cat is then read from
 * build/x86_64/utils/cat. With no map, or no FROM that matches, the path is
 * read as it is written. A mapped path is in one of four static buffers, so
 * it is good until the fourth call after this one; one that does not fit
 * ends the program.
 */
static const char *host_path(const char *path)
{
    static char buf[4][1024];
    static int next;
    const char *map = getenv("RACH2_PATH_MAP");
    const char *p;

    if (map == NULL) {
        return path;
    }
    for (p = map; *p != '\0';) {
        const char *word;
        const char *eq;
        size_t from_len;
        size_t to_len;

        while (*p == ' ' || *p == '\t') {
            p++;
        }
        word = p;
        while (*p != '\0' && *p != ' ' && *p != '\t') {
            p++;
        }
        eq = memchr(word, '=', (size_t)(p - word));
        if (eq == NULL) {
            continue;
        }
        from_len = (size_t)(eq - word);
        to_len = (size_t)(p - eq - 1);
        if (from_len > 0 && strncmp(path, word, from_len) == 0) {
            char *out = buf[next];

            next = (next + 1) % 4;
            if (snprintf(out, sizeof(buf[0]), "%.*s%s", (int)to_len, eq + 1, path + from_len) >=
                (int)sizeof(buf[0])) {
                fprintf(stderr, "stageimage: the mapped path of '%s' is too long.\n", path);
                exit(1);
            }

            return out;
        }
    }

    return path;
}

/**
 * Returns the file `STAGEIMAGE_DEPS` names, opened for writing on the first
 * call, or NULL when it names none or cannot be opened.
 */
static FILE *deps_file(void)
{
    static FILE *file;
    static int tried;

    if (!tried) {
        const char *name = getenv("STAGEIMAGE_DEPS");

        tried = 1;
        if (name != NULL && *name != '\0') {
            file = fopen(name, "w");
        }
    }
    return file;
}

/**
 * Writes `path`, a host file or directory the stager reads, as a line of the
 * `STAGEIMAGE_DEPS` file when there is one. The path of an absent
 * `optional-file` is written too. krffs/mkimage.sh makes the lines into make
 * dependencies, so a changed, added or removed input restages the images
 * that read it.
 */
static void record_input(const char *path)
{
    if (deps_file() != NULL) {
        fprintf(deps_file(), "%s\n", path);
    }
}

/**
 * Writes a directory `exec-dir` stages from as a line of `exec-dir`, a tab
 * and the path. The build judges such a directory by the programs in it
 * (krffs/programs.sh) and not by its own time, which every file made beside
 * the programs moves.
 */
static void record_exec_dir(const char *path)
{
    if (deps_file() != NULL) {
        fprintf(deps_file(), "exec-dir\t%s\n", path);
    }
}

/**
 * Reads the host file `path` whole into a buffer from `malloc`, which the
 * caller frees, sets `*len` to its length, and records the path with
 * `record_input`. A NUL follows the data and is not counted. Returns NULL
 * when the file cannot be opened or read.
 */
static char *read_whole_file(const char *path, size_t *len)
{
    FILE *f;

    record_input(path);
    f = fopen(path, "rb");
    long n;
    char *buf;

    if (!f) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    buf = (char *)malloc((size_t)n + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    if ((size_t)n != 0 && fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    buf[n] = '\0';
    *len = (size_t)n;
    return buf;
}

/**
 * Writes the guest path of the directory `node` into `buf`, of `size` bytes,
 * for a message.
 *
 * The path is found by the parents' ids, up to the root, whose id is 0. A
 * chain deeper than the walk keeps, or a parent that is not there, ends the
 * path at what was found, and a path longer than `size` is cut.
 */
static void dir_path(struct krffs_file_system *file_system, const struct krffs_node *node,
                     char *buf, size_t size)
{
    const struct krffs_node *chain[64];
    size_t depth = 0;
    size_t used = 0;

    while (node != NULL && depth < sizeof(chain) / sizeof(chain[0])) {
        chain[depth++] = node;
        node =
            node->parent_id == 0 ? NULL : krffs_find_directory_by_id(file_system, node->parent_id);
    }
    buf[0] = '\0';
    while (depth > 0 && used < size) {
        int n = snprintf(buf + used, size - used, "/%s", (const char *)chain[--depth]->name);

        if (n < 0) {
            break;
        }
        used += (size_t)n;
    }
}

/**
 * Returns whether no directory has the id that `name` in `parent_id` mixes
 * to, and when one has, reports that the directory `path` would have its id.
 *
 * Two names can mix to one id: `bC` and `cb` have one djb2 hash. A child
 * records its parent by id alone, so a second directory with a taken id would
 * share its children with the first. The kernel does not make one
 * (`krffs_id_taken` in rach2/sys/fs/krffs.c), and an image with one would give
 * it a hierarchy the kernel never builds, so the stager does not either.
 */
static bool dir_id_free(struct krffs_file_system *file_system, uint64_t parent_id, const char *name,
                        const char *path)
{
    const struct krffs_node *other =
        krffs_find_directory_by_id(file_system, krffs_mix_id(parent_id, name));
    char other_path[512];

    if (other == NULL) {
        return true;
    }
    dir_path(file_system, other, other_path, sizeof(other_path));
    fprintf(stderr,
            "stageimage: directory '%s' has the id of directory '%s'; the two would "
            "share their children.\n",
            path, other_path);

    return false;
}

/**
 * Returns the sum of the sizes of the free nodes, headers included, which the
 * summary line reports.
 */
static uint64_t total_free_space(struct krffs_file_system *file_system)
{
    struct krffs_node *node = file_system->node;
    uint64_t free_bytes = 0;

    do {
        if (node->type == KRFFS_Free_Node) {
            free_bytes += node->size;
        }
    } while ((node = krffs_get_next_node(file_system, node)) != file_system->node);

    return free_bytes;
}

/**
 * Reserves a node named `name` in the directory `parent_id`, with `mode`, the
 * owner `uid`:`gid`, `data_size` bytes of `data`, none copied when `data` is
 * NULL, and the fixed staged time as its three times, and syncs it. Its id is
 * the one the kernel gives the same node: the djb2 hash of the name for a
 * file, or the hash mixed with `parent_id` by `krffs_mix_id` for a directory.
 * Returns the node, or NULL, with a message, when the data is too large or no
 * free node is large enough.
 *
 * The staged time is one second past the epoch: fixed, so that an image
 * built twice from one tree is one image, and not zero, since pdpmake takes a
 * modification time of zero for a file that does not exist, and make must
 * run over the staged sources in place. A file the machine builds gets the
 * time of its clock, which is later.
 */
static struct krffs_node *stage_node(struct krffs_file_system *file_system, uint64_t parent_id,
                                     const char *name, uint16_t mode, uint32_t uid, uint32_t gid,
                                     const char *data, uint64_t data_size)
{
    struct krffs_node *node;
    struct krffs_node_time staged_time = {1, 0};

    if (data_size > (uint64_t)SIZE_MAX) {
        fprintf(stderr, "stageimage: '%s' is too large to stage.\n", name);

        return NULL;
    }

    node = krffs_allocate_reserved_node(file_system, data_size, NULL);

    if (!node) {
        fprintf(stderr, "stageimage: not enough space for '%s'.\n", name);

        return NULL;
    }

    krffs_set_reserved_node_name(node, name);

    node->parent_id = parent_id;
    node->mode = mode;
    if (is_dir_node(node)) {
        node->id = krffs_mix_id(parent_id, name);
    }
    node->uid = uid;
    node->gid = gid;
    node->data_size = data_size;

    node->access_time = staged_time;
    node->modification_time = staged_time;
    node->attribute_change_time = staged_time;

    if (data_size > 0 && data) {
        memcpy((char *)(node + 1), data, (size_t)data_size);
    }

    krffs_sync_mapping(node, sizeof(*node) + (size_t)data_size);

    return node;
}

/**
 * Returns the id of the directory at the absolute guest `path`, 0 for `/`,
 * and makes each missing component a root-owned 0755 directory; a component
 * already there keeps its mode and owner. Returns `(uint64_t)-1`, with a
 * message, for a path that is not absolute or has 256 bytes or more, a
 * component that is not a directory, since a manifest cannot put a file under
 * a file, a directory id that is taken, or a full image.
 */
static uint64_t resolve_dir(struct krffs_file_system *file_system, const char *path)
{
    char buf[256];
    uint64_t parent = 0;
    char *p;
    char *save = NULL;

    if (path[0] != '/') {
        fprintf(stderr, "stageimage: directory path '%s' is not absolute.\n", path);

        return (uint64_t)-1;
    }
    if (path[1] == '\0') {
        return 0;
    }
    if (strlen(path) >= sizeof(buf)) {
        fprintf(stderr, "stageimage: path '%s' is too long.\n", path);

        return (uint64_t)-1;
    }
    strcpy(buf, path + 1);

    for (p = strtok_r(buf, "/", &save); p != NULL; p = strtok_r(NULL, "/", &save)) {
        struct krffs_node *existing = krffs_find_child(file_system, parent, p);
        uint64_t id;

        if (existing != NULL) {
            if (!is_dir_node(existing)) {
                fprintf(stderr, "stageimage: path '%s' descends through non-directory '%s'.\n",
                        path, p);

                return (uint64_t)-1;
            }
            id = existing->id;
        } else {
            char here[256];
            struct krffs_node *made;

            snprintf(here, sizeof(here), "%.*s", (int)(1 + (p - buf) + strlen(p)), path);
            if (!dir_id_free(file_system, parent, p, here)) {
                return (uint64_t)-1;
            }
            made = stage_node(file_system, parent, p, (uint16_t)(KRFFS_MODE_IFDIR | 0755), 0, 0,
                              NULL, 0);
            if (made == NULL) {
                return (uint64_t)-1;
            }
            id = made->id;
        }
        parent = id;
    }

    return parent;
}

/**
 * Splits the absolute guest `path` into its directory, written to `dir_out`
 * ("/" for a name in the root), and its last component, returned as a
 * pointer into `path_buf`, which gets a copy of `path`. Returns NULL when
 * `path` is not absolute, ends in a slash, or does not fit.
 */
static char *split_path(const char *path, char *path_buf, size_t bufsz, char *dir_out, size_t dirsz)
{
    char *slash;

    if (path[0] != '/' || strlen(path) >= bufsz) {
        return NULL;
    }
    strcpy(path_buf, path);
    slash = strrchr(path_buf, '/');
    if (slash == NULL || slash[1] == '\0') {
        return NULL;
    }
    if (slash == path_buf) {
        if (dirsz < 2) {
            return NULL;
        }
        strcpy(dir_out, "/");
    } else {
        size_t dlen = (size_t)(slash - path_buf);

        if (dlen >= dirsz) {
            return NULL;
        }
        memcpy(dir_out, path_buf, dlen);
        dir_out[dlen] = '\0';
    }

    return slash + 1;
}

/**
 * Returns whether `path` is /dev or /proc or under one. The kernel makes those
 * with devfs and procfs, and a KRFFS node there is never seen, so a manifest
 * stages nothing there.
 */
static bool is_synthetic_path(const char *path)
{
    return strcmp(path, "/dev") == 0 || strncmp(path, "/dev/", 5) == 0 ||
           strcmp(path, "/proc") == 0 || strncmp(path, "/proc/", 6) == 0;
}

/**
 * The directory of the tree mode, `stageimage.krffs MANIFEST --tree DIR`, or
 * NULL when the stager writes an image. In the tree mode each node becomes a
 * host file or directory under DIR, and a line `node MODE UID GID PATH`, with
 * the POSIX type bits in MODE, goes to stdout for each. tools/image/mkext2.sh
 * gives the directory to mke2fs and the lines to debugfs, which sets the
 * owners and the set-id bits a build that is not root cannot put on a host
 * file. Every node gets the fixed staged time, so that one manifest makes one
 * image.
 */
static const char *tree_root;
/** The lines `tree_note` has printed. */
static unsigned long tree_nodes;

/** Prints the `node` line of the guest `path` and counts it. */
static void tree_note(uint16_t mode, uint32_t uid, uint32_t gid, const char *path)
{
    printf("node 0%o %lu %lu %s\n", (unsigned)mode, (unsigned long)uid, (unsigned long)gid, path);
    tree_nodes++;
}

/**
 * Makes the host directory `host_path` and its missing parents. A directory
 * made under `tree_root` is a node too, and is noted as root's and 0755, so
 * that the image does not keep the builder's owner and umask on it; the last
 * component only when `note_last`, since a caller that names the directory
 * itself notes it with its own mode. Returns false when the path does not fit
 * or a `mkdir` fails other than with `EEXIST`.
 */
static bool tree_mkdir_p(const char *host_path, bool note_last)
{
    char buf[1024];
    size_t i, skip = strlen(tree_root);

    if (strlen(host_path) >= sizeof(buf)) {
        return false;
    }
    strcpy(buf, host_path);
    for (i = 1; buf[i] != '\0'; i++) {
        if (buf[i] == '/') {
            buf[i] = '\0';
            if (mkdir(buf, 0755) == 0) {
                if (i > skip) {
                    tree_note((uint16_t)(S_IFDIR | 0755), 0, 0, buf + skip);
                }
            } else if (errno != EEXIST) {
                return false;
            }
            buf[i] = '/';
        }
    }
    if (mkdir(buf, 0755) == 0) {
        if (note_last && strlen(buf) > skip) {
            tree_note((uint16_t)(S_IFDIR | 0755), 0, 0, buf + skip);
        }
        return true;
    }
    return errno == EEXIST;
}

/**
 * Makes the directory `path` under `tree_root` and notes it with `mode` and
 * the owner `uid`:`gid`, also when it was there already. Returns false, with a
 * message, when it cannot be made.
 */
static bool tree_dir(const char *path, uint16_t mode, uint32_t uid, uint32_t gid)
{
    char host[1024];

    if (snprintf(host, sizeof(host), "%s%s", tree_root, path) >= (int)sizeof(host) ||
        !tree_mkdir_p(host, false)) {
        fprintf(stderr, "stageimage: cannot make '%s%s'.\n", tree_root, path);

        return false;
    }
    tree_note((uint16_t)(S_IFDIR | mode), uid, gid, path);

    return true;
}

/**
 * Writes `data_size` bytes of `data` as the host file of `path` under
 * `tree_root`, making its directories, and notes it with `mode` and the owner
 * `uid`:`gid`. A file already there is the same path staged twice, as in an
 * image; the create with `O_EXCL` and `O_NOFOLLOW` finds that in one step and
 * follows no link. Returns false, with a message, for that or a failed write.
 */
static bool tree_file(const char *path, uint16_t mode, uint32_t uid, uint32_t gid, const char *data,
                      uint64_t data_size)
{
    char host[1024];
    char *slash;
    FILE *out;
    int fd;

    if (snprintf(host, sizeof(host), "%s%s", tree_root, path) >= (int)sizeof(host)) {
        fprintf(stderr, "stageimage: path too long '%s'.\n", path);

        return false;
    }
    slash = strrchr(host, '/');
    *slash = '\0';
    if (!tree_mkdir_p(host, true)) {
        fprintf(stderr, "stageimage: cannot make '%s'.\n", host);

        return false;
    }
    *slash = '/';
    fd = open(host, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0644);
    if (fd < 0 && errno == EEXIST) {
        fprintf(stderr, "stageimage: '%s' is staged more than once.\n", path);

        return false;
    }
    out = fd < 0 ? NULL : fdopen(fd, "wb");
    if (out == NULL || (data_size != 0 && fwrite(data, 1, (size_t)data_size, out) != data_size) ||
        fclose(out) != 0) {
        fprintf(stderr, "stageimage: cannot write '%s'.\n", host);

        return false;
    }
    tree_note((uint16_t)(S_IFREG | mode), uid, gid, path);

    return true;
}

/**
 * Sets the access and modification times of `host_path`, and of everything
 * under it, to the fixed staged time, following no link. A directory is set
 * after what is in it, since a write in a directory changes its time.
 * Returns false when a node cannot be read or set.
 */
static bool tree_stamp(const char *host_path)
{
    DIR *d;
    struct dirent *e;
    struct stat st;
    struct timespec times[2];
    bool ok = true;

    times[0].tv_sec = 1;
    times[0].tv_nsec = 0;
    times[1] = times[0];
    if (lstat(host_path, &st) != 0) {
        return false;
    }
    if (S_ISDIR(st.st_mode)) {
        d = opendir(host_path);
        if (d == NULL) {
            return false;
        }
        while ((e = readdir(d)) != NULL) {
            char child[1024];

            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
                continue;
            }
            if (snprintf(child, sizeof(child), "%s/%s", host_path, e->d_name) >=
                    (int)sizeof(child) ||
                !tree_stamp(child)) {
                ok = false;
            }
        }
        closedir(d);
    }

    return ok && utimensat(AT_FDCWD, host_path, times, AT_SYMLINK_NOFOLLOW) == 0;
}

/**
 * Stages the directory `path` with `mode` and the owner `uid`:`gid`, making
 * missing parents as `resolve_dir` does. A directory already at `path` gets
 * the mode and owner too, so a `dir 0700:1000:1000 /home/test` line counts
 * whether it comes before or after a line that puts something inside; `/` is
 * left as it is. Returns false, with a message, for a bad path, a path under
 * /dev or /proc, a file at `path`, an id that is taken or a full image.
 */
static bool stage_dir(struct krffs_file_system *file_system, const char *path, uint16_t mode,
                      uint32_t uid, uint32_t gid)
{
    char path_buf[256];
    char dir_buf[256];
    char *name;
    uint64_t parent_id;
    struct krffs_node *node;

    if (strcmp(path, "/") == 0) {
        return true;
    }
    if (!is_valid_guest_path(path)) {
        fprintf(stderr, "stageimage: malformed directory path '%s'.\n", path);

        return false;
    }
    if (is_synthetic_path(path)) {
        fprintf(stderr, "stageimage: '%s' is under a kernel-synthetic namespace.\n", path);

        return false;
    }
    if (tree_root != NULL) {
        return tree_dir(path, mode, uid, gid);
    }

    name = split_path(path, path_buf, sizeof(path_buf), dir_buf, sizeof(dir_buf));
    if (name == NULL) {
        fprintf(stderr, "stageimage: malformed directory path '%s'.\n", path);

        return false;
    }

    parent_id = resolve_dir(file_system, dir_buf);
    if (parent_id == (uint64_t)-1) {
        return false;
    }

    node = krffs_find_child(file_system, parent_id, name);
    if (node != NULL) {
        if (!is_dir_node(node)) {
            fprintf(stderr, "stageimage: '%s' already exists as a non-directory.\n", path);

            return false;
        }
        node->mode = (uint16_t)(KRFFS_MODE_IFDIR | mode);
        node->uid = uid;
        node->gid = gid;
        krffs_sync_mapping(node, sizeof(*node));

        return true;
    }
    if (!dir_id_free(file_system, parent_id, name, path)) {
        return false;
    }

    return stage_node(file_system, parent_id, name, (uint16_t)(KRFFS_MODE_IFDIR | mode), uid, gid,
                      NULL, 0) != NULL;
}

/**
 * Stages the file `path` with `mode`, the owner `uid`:`gid` and `data_size`
 * bytes of `data`, making missing directories as `resolve_dir` does. Each
 * image is staged from an empty file system, so a node already at `path`
 * means the manifest stages the path twice, which is an error. Returns false,
 * with a message, for that, a bad path, a path under /dev or /proc, or a full
 * image.
 */
static bool stage_file(struct krffs_file_system *file_system, const char *path, uint16_t mode,
                       uint32_t uid, uint32_t gid, const char *data, uint64_t data_size)
{
    char path_buf[256];
    char dir_buf[256];
    char *name;
    uint64_t dir_id;

    if (!is_valid_guest_path(path) || strcmp(path, "/") == 0) {
        fprintf(stderr, "stageimage: malformed file path '%s'.\n", path);

        return false;
    }
    if (is_synthetic_path(path)) {
        fprintf(stderr, "stageimage: '%s' is under a kernel-synthetic namespace.\n", path);

        return false;
    }
    if (tree_root != NULL) {
        return tree_file(path, mode, uid, gid, data, data_size);
    }

    name = split_path(path, path_buf, sizeof(path_buf), dir_buf, sizeof(dir_buf));
    if (name == NULL) {
        fprintf(stderr, "stageimage: malformed file path '%s'.\n", path);

        return false;
    }

    dir_id = resolve_dir(file_system, dir_buf);
    if (dir_id == (uint64_t)-1) {
        return false;
    }

    if (krffs_find_child(file_system, dir_id, name) != NULL) {
        fprintf(stderr, "stageimage: '%s' is staged more than once.\n", path);

        return false;
    }

    return stage_node(file_system, dir_id, name, mode, uid, gid, data, data_size) != NULL;
}

/**
 * Reads a mode field, octal permission bits up to 07777 with an optional
 * decimal owner, `0755` or `0700:1000:1000`, into `*out`, `*uid` and `*gid`;
 * the owner is 0:0 when none is given. Returns false for a malformed field.
 */
static bool parse_mode(const char *s, uint16_t *out, uint32_t *uid, uint32_t *gid)
{
    char *end;
    unsigned long v = strtoul(s, &end, 8);

    if (*s == '\0' || v > 07777u || (*end != '\0' && *end != ':')) {
        return false;
    }
    *out = (uint16_t)v;
    *uid = 0;
    *gid = 0;

    if (*end == ':') {
        const char *start;
        unsigned long long u;
        unsigned long long g;

        start = end + 1;
        u = strtoull(start, &end, 10);
        if (end == start || *end != ':') {
            return false;
        }
        start = end + 1;
        g = strtoull(start, &end, 10);
        if (end == start || *end != '\0' || u > 0xFFFFFFFFull || g > 0xFFFFFFFFull) {
            return false;
        }
        *uid = (uint32_t)u;
        *gid = (uint32_t)g;
    }

    return true;
}

/**
 * Replaces the escapes \n, \t and \\ of a `text` payload in place, keeps any
 * other backslash as it is, and returns the new length. A `text` directive
 * is one line of the manifest, so a file of several lines, such as
 * /etc/hosts, is written with \n.
 */
static size_t unescape_text(char *s)
{
    char *r = s;
    char *w = s;

    while (*r) {
        if (*r == '\\' && r[1]) {
            r++;
            switch (*r) {
            case 'n':
                *w++ = '\n';
                break;
            case 't':
                *w++ = '\t';
                break;
            case '\\':
                *w++ = '\\';
                break;
            default:
                *w++ = '\\';
                *w++ = *r;
                break;
            }
            r++;
        } else {
            *w++ = *r++;
        }
    }
    *w = '\0';

    return (size_t)(w - s);
}

/**
 * Orders two directory entries by name, for the `qsort` of `stage_exec_dir`
 * and `stage_tree`.
 */
static int compare_names(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/**
 * Stages each regular file with an execute bit directly in the host
 * directory `hostdir` into the guest directory `guestdir`, with `mode` and the
 * owner `uid`:`gid`, so that a manifest stages a built set, such as the
 * programs in build/utils, without naming each.
 *
 * The walk does not descend. A name that begins with `.` is skipped, and so
 * is anything that `lstat` shows is not a regular file with an execute bit,
 * such as a symbolic link or a `.d` file the compiler writes beside the
 * programs.
 * The names are sorted before they are staged, since `readdir` order is the
 * host file system's, a hash of the name on APFS, and one host directory must
 * make one image. The directory is recorded with `record_exec_dir`. Returns
 * false, with a message, on a host error or a staging error.
 */
static bool stage_exec_dir(struct krffs_file_system *file_system, const char *guestdir,
                           const char *hostdir, uint16_t mode, uint32_t uid, uint32_t gid)
{
    DIR *d;
    struct dirent *e;
    char **names = NULL;
    size_t count = 0;
    size_t i;
    bool ok = true;

    record_exec_dir(hostdir);
    d = opendir(hostdir);
    if (d == NULL) {
        fprintf(stderr, "stageimage: cannot open host dir '%s'.\n", hostdir);

        return false;
    }

    errno = 0;
    while ((e = readdir(d)) != NULL) {
        char **grown;

        if (e->d_name[0] == '.') {
            continue;
        }
        grown = realloc(names, (count + 1) * sizeof(*names));
        if (grown == NULL) {
            ok = false;
            break;
        }
        names = grown;
        names[count] = strdup(e->d_name);
        if (names[count] == NULL) {
            ok = false;
            break;
        }
        count++;
        errno = 0;
    }
    /* readdir returns NULL both at the end and on an error. */
    if (errno != 0) {
        fprintf(stderr, "stageimage: cannot read host dir '%s'.\n", hostdir);
        ok = false;
    }
    closedir(d);
    if (ok) {
        qsort(names, count, sizeof(*names), compare_names);
    }

    for (i = 0; ok && i < count; i++) {
        char hostpath[1024];
        char guestpath[512];
        struct stat st;
        char *data;
        size_t len = 0;
        int n;

        n = snprintf(hostpath, sizeof(hostpath), "%s/%s", hostdir, names[i]);
        if (n < 0 || (size_t)n >= sizeof(hostpath)) {
            fprintf(stderr, "stageimage: host path under '%s' is too long.\n", hostdir);
            ok = false;
            break;
        }
        if (lstat(hostpath, &st) != 0 || !S_ISREG(st.st_mode) ||
            !(st.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH))) {
            continue;
        }

        data = read_whole_file(hostpath, &len);
        if (data == NULL) {
            fprintf(stderr, "stageimage: cannot read '%s'.\n", hostpath);
            ok = false;
            break;
        }

        if (strcmp(guestdir, "/") == 0) {
            n = snprintf(guestpath, sizeof(guestpath), "/%s", names[i]);
        } else {
            n = snprintf(guestpath, sizeof(guestpath), "%s/%s", guestdir, names[i]);
        }
        if (n < 0 || (size_t)n >= sizeof(guestpath)) {
            fprintf(stderr, "stageimage: guest path under '%s' is too long.\n", guestdir);
            free(data);
            ok = false;
            break;
        }
        ok = stage_file(file_system, guestpath, mode, uid, gid, data, len);
        free(data);
    }

    for (i = 0; i < count; i++) {
        free(names[i]);
    }
    free(names);

    return ok;
}

/**
 * Makes the directory `path` with `stage_dir` when nothing is there, and
 * leaves a directory already there as it is. `stage_dir` sets the mode and
 * owner of a directory it finds, as a `dir` line asks; a `tree` walks into
 * directories other lines made, and a tree owned by uid 1000 under a
 * root-owned 0700 directory would otherwise give that directory away.
 * Returns false, with a message, when a file is at `path` or `stage_dir`
 * fails.
 */
static bool stage_dir_if_absent(struct krffs_file_system *file_system, const char *path,
                                uint16_t mode, uint32_t uid, uint32_t gid)
{
    char path_buf[256];
    char dir_buf[256];
    char *name;
    uint64_t parent_id;
    struct krffs_node *node;

    if (strcmp(path, "/") == 0) {
        return true;
    }
    if (tree_root != NULL) {
        char host[1024];
        struct stat st;

        if (snprintf(host, sizeof(host), "%s%s", tree_root, path) >= (int)sizeof(host)) {
            return false;
        }
        if (lstat(host, &st) == 0) {
            if (!S_ISDIR(st.st_mode)) {
                fprintf(stderr, "stageimage: '%s' already exists as a non-directory.\n", path);

                return false;
            }

            return true;
        }

        return stage_dir(file_system, path, mode, uid, gid);
    }
    name = split_path(path, path_buf, sizeof(path_buf), dir_buf, sizeof(dir_buf));
    if (name == NULL) {
        fprintf(stderr, "stageimage: malformed directory path '%s'.\n", path);

        return false;
    }
    parent_id = resolve_dir(file_system, dir_buf);
    if (parent_id == (uint64_t)-1) {
        return false;
    }
    node = krffs_find_child(file_system, parent_id, name);
    if (node != NULL) {
        if (!is_dir_node(node)) {
            fprintf(stderr, "stageimage: '%s' already exists as a non-directory.\n", path);

            return false;
        }

        return true;
    }

    return stage_dir(file_system, path, mode, uid, gid);
}

/**
 * Stages each regular file under the host directory `hostdir` into the guest
 * directory `guestdir`, with `mode` and the owner `uid`:`gid`, descending into
 * each directory. It is for a tree of sources, such as the musl headers,
 * which a manifest could not name one by one; `exec-dir` stages the programs
 * of one directory. The rules are fixed:
 *
 *   * `mode` is the mode of each file. A directory the walk makes gets 0755
 *     and the same owner, since a directory needs its execute bit to be
 *     searched and a header does not; a directory already there is left as
 *     it is (`stage_dir_if_absent`).
 *   * Only regular files and directories are staged. Anything else under
 *     `hostdir`, a symbolic link, a device or a socket, is an error: KRFFS
 *     has no symbolic links, and dropping one would make the image differ
 *     from the tree without a word.
 *   * An empty file is staged as an empty file.
 *   * A name that begins with `.` is skipped, as in `exec-dir`.
 *   * The names are sorted, so one host tree makes one image.
 *   * `hostdir` must be a directory and not a link to one, which would walk
 *     a tree the manifest did not name.
 *
 * Every directory walked is recorded with `record_input`. Returns false,
 * with a message, on any error.
 */
static bool stage_tree(struct krffs_file_system *file_system, const char *guestdir,
                       const char *hostdir, uint16_t mode, uint32_t uid, uint32_t gid)
{
    struct dirent *e;
    struct stat root_st;
    char **names = NULL;
    size_t count = 0;
    size_t i;
    bool ok = true;
    DIR *d;

    record_input(hostdir);
    d = opendir(hostdir);
    if (d == NULL) {
        fprintf(stderr, "stageimage: cannot open host dir '%s'.\n", hostdir);

        return false;
    }

    if (lstat(hostdir, &root_st) != 0 || !S_ISDIR(root_st.st_mode)) {
        fprintf(stderr,
                "stageimage: '%s' is not a directory; 'tree' takes "
                "one, and follows no symlink to reach it.\n",
                hostdir);
        closedir(d);

        return false;
    }

    errno = 0;
    while ((e = readdir(d)) != NULL) {
        char **grown;

        if (e->d_name[0] == '.') {
            continue;
        }
        grown = realloc(names, (count + 1) * sizeof(*names));
        if (grown == NULL) {
            ok = false;
            break;
        }
        names = grown;
        names[count] = strdup(e->d_name);
        if (names[count] == NULL) {
            ok = false;
            break;
        }
        count++;
        errno = 0;
    }
    /* readdir returns NULL both at the end and on an error. */
    if (errno != 0) {
        fprintf(stderr, "stageimage: cannot read host dir '%s'.\n", hostdir);
        ok = false;
    }
    closedir(d);
    if (ok) {
        qsort(names, count, sizeof(*names), compare_names);
    }

    if (!stage_dir_if_absent(file_system, guestdir, 0755, uid, gid)) {
        ok = false;
    }

    for (i = 0; ok && i < count; i++) {
        char hostpath[1024];
        char guestpath[512];
        struct stat st;
        char *data;
        size_t len = 0;
        int n;

        n = snprintf(hostpath, sizeof(hostpath), "%s/%s", hostdir, names[i]);
        if (n < 0 || (size_t)n >= sizeof(hostpath)) {
            fprintf(stderr, "stageimage: host path under '%s' is too long.\n", hostdir);
            ok = false;
            break;
        }
        if (strcmp(guestdir, "/") == 0) {
            n = snprintf(guestpath, sizeof(guestpath), "/%s", names[i]);
        } else {
            n = snprintf(guestpath, sizeof(guestpath), "%s/%s", guestdir, names[i]);
        }
        if (n < 0 || (size_t)n >= sizeof(guestpath)) {
            fprintf(stderr, "stageimage: guest path under '%s' is too long.\n", guestdir);
            ok = false;
            break;
        }

        if (lstat(hostpath, &st) != 0) {
            fprintf(stderr, "stageimage: cannot stat '%s'.\n", hostpath);
            ok = false;
            break;
        }
        if (S_ISDIR(st.st_mode)) {
            ok = stage_tree(file_system, guestpath, hostpath, mode, uid, gid);
            continue;
        }
        if (!S_ISREG(st.st_mode)) {
            fprintf(stderr,
                    "stageimage: '%s' is neither a regular file nor a "
                    "directory; 'tree' stages no other kind.\n",
                    hostpath);
            ok = false;
            break;
        }

        data = read_whole_file(hostpath, &len);
        if (data == NULL) {
            fprintf(stderr, "stageimage: cannot read '%s'.\n", hostpath);
            ok = false;
            break;
        }
        ok = stage_file(file_system, guestpath, mode, uid, gid, data, len);
        free(data);
    }

    for (i = 0; i < count; i++) {
        free(names[i]);
    }
    free(names);

    return ok;
}

/**
 * Applies one manifest line, whose fields are split at blanks. A blank line
 * and a line whose first field begins with `#` are skipped. MODE is octal
 * permission bits with an optional owner, `0755` or `0700:1000:1000`, root
 * when none is given, and PATH is an absolute guest path:
 *
 *     dir           MODE PATH           a directory
 *     file          MODE PATH HOSTPATH  a file copied from the host
 *     optional-file MODE PATH HOSTPATH  the same, skipped when HOSTPATH
 *                                       does not exist
 *     text          MODE PATH TEXT      a file of the rest of the line, with
 *                                       \n, \t and \\ replaced
 *     exec-dir      MODE PATH HOSTDIR   the programs directly in HOSTDIR
 *     tree          MODE PATH HOSTDIR   the regular files under HOSTDIR
 *
 * A host path goes through `host_path`. `optional-file` skips only a host
 * file that does not exist; one that exists and cannot be read is an error,
 * as for `file`. A `tree` line with a set-user-ID or set-group-ID mode is an
 * error, since the mode would give privilege to whatever is in the host tree.
 *
 * Returns true, or false, with a message, for a malformed line, an unknown
 * directive or a staging error.
 */
static bool apply_line(struct krffs_file_system *file_system, char *line)
{
    char *save = NULL;
    char *verb = strtok_r(line, " \t", &save);
    char *mode_s;
    char *path;
    uint16_t mode;
    uint32_t uid;
    uint32_t gid;

    if (verb == NULL || verb[0] == '#') {
        return true;
    }

    mode_s = strtok_r(NULL, " \t", &save);
    path = strtok_r(NULL, " \t", &save);
    if (mode_s == NULL || path == NULL || !parse_mode(mode_s, &mode, &uid, &gid)) {
        fprintf(stderr, "stageimage: malformed '%s' directive.\n", verb);

        return false;
    }

    if (strcmp(verb, "dir") == 0) {
        return stage_dir(file_system, path, mode, uid, gid);
    }
    if (strcmp(verb, "text") == 0) {
        char *text = strtok_r(NULL, "", &save);
        size_t len;

        if (text == NULL) {
            text = (char *)"";
        }
        while (*text == ' ' || *text == '\t') {
            text++;
        }
        len = unescape_text(text);

        return stage_file(file_system, path, mode, uid, gid, text, len);
    }
    if (strcmp(verb, "file") == 0 || strcmp(verb, "optional-file") == 0) {
        const char *hostpath = strtok_r(NULL, " \t", &save);
        char *data;
        size_t len = 0;
        bool ok;

        if (hostpath == NULL) {
            fprintf(stderr, "stageimage: '%s' needs a host path.\n", verb);

            return false;
        }
        hostpath = host_path(hostpath);
        record_input(hostpath);
        if (access(hostpath, F_OK) != 0) {
            if (strcmp(verb, "optional-file") == 0) {
                printf("stageimage: optional '%s' absent (%s); skipping.\n", path, hostpath);

                return true;
            }
            fprintf(stderr, "stageimage: '%s' does not exist.\n", hostpath);

            return false;
        }
        data = read_whole_file(hostpath, &len);
        if (data == NULL) {
            fprintf(stderr, "stageimage: cannot read '%s'.\n", hostpath);

            return false;
        }
        ok = stage_file(file_system, path, mode, uid, gid, data, len);
        free(data);

        return ok;
    }
    if (strcmp(verb, "tree") == 0) {
        const char *hostdir = strtok_r(NULL, " \t", &save);

        if (hostdir == NULL) {
            fprintf(stderr, "stageimage: 'tree' needs a host dir.\n");

            return false;
        }
        hostdir = host_path(hostdir);
        if (mode & 06000) {
            fprintf(stderr,
                    "stageimage: 'tree' stages mode %04o into %s; a "
                    "set-id tree privileges whatever is in the host "
                    "directory. Name the one binary with 'file'.\n",
                    mode, path);

            return false;
        }

        return stage_tree(file_system, path, hostdir, mode, uid, gid);
    }
    if (strcmp(verb, "exec-dir") == 0) {
        const char *hostdir = strtok_r(NULL, " \t", &save);

        if (hostdir == NULL) {
            fprintf(stderr, "stageimage: 'exec-dir' needs a host dir.\n");

            return false;
        }
        hostdir = host_path(hostdir);

        return stage_exec_dir(file_system, path, hostdir, mode, uid, gid);
    }

    fprintf(stderr, "stageimage: unknown directive '%s'.\n", verb);

    return false;
}

/**
 * stageimage.krffs MANIFEST IMAGE [--offset BYTES --size BYTES], or
 * stageimage.krffs MANIFEST --tree DIRECTORY: stages each line of MANIFEST
 * with `apply_line`, into the KRFFS region of IMAGE, which `mkfs.krffs` has
 * made, or into DIRECTORY, which must not exist yet, so that nothing an
 * earlier run left can end up in an image.
 *
 * The image is written without mounting it, directories and files alike.
 * Its root gets the owner 0:0 and the fixed staged time in place of the
 * builder's and the time of `mkfs.krffs`, so that an image built twice from
 * one tree is one image, whoever builds it. Only directories are found or
 * made: a file whose path is already in the image fails as staged more than
 * once, so a run over a partly staged image stops at the first file it
 * staged before. A manifest line longer than 2046 bytes, or a full image,
 * fails with a message. Host paths are read from the current directory, so
 * the build runs the stager from the top of the tree. At the end one line on
 * stdout gives the bytes used and free, or the nodes of the tree.
 *
 * Returns `EXIT_SUCCESS` or `EXIT_FAILURE`.
 */
int main(int argc, char **argv)
{
    int exit_status = EXIT_SUCCESS;
    int file_descriptor = -1;
    struct krffs_file_system file_system = {.node = NULL};
    struct stat file_info;
    FILE *manifest;
    char line[2048];
    uint64_t free_after;

    if (argc == 4 && strcmp(argv[2], "--tree") == 0 && argv[1][0] != '-') {
        if (mkdir(argv[3], 0755) != 0) {
            fprintf(stderr, "stageimage: cannot make '%s' (it must not exist): %s.\n", argv[3],
                    strerror(errno));

            return EXIT_FAILURE;
        }
        tree_root = argv[3];
    } else if (argc < 3 || argv[1][0] == '-' || argv[2][0] == '-') {
        fprintf(stderr,
                "Usage: %s <manifest> <image> [--offset BYTES --size BYTES]\n"
                "       %s <manifest> --tree <directory>\n",
                argc >= 1 ? argv[0] : "stageimage.krffs", argc >= 1 ? argv[0] : "stageimage.krffs");

        return EXIT_FAILURE;
    }

    if (tree_root != NULL) {
        goto stage;
    }

    if ((file_descriptor = PLATFORM_PREFIX(open(argv[2], O_RDWR))) == -1) {
        fprintf(stderr, "stageimage: cannot open '%s'.\n", argv[2]);

        return EXIT_FAILURE;
    }

    if (fstat(file_descriptor, &file_info) == -1 || !S_ISREG(file_info.st_mode)) {
        fprintf(stderr, "stageimage: '%s' is not a regular file.\n", argv[2]);

        exit_status = EXIT_FAILURE;

        goto cleanup;
    }

    off_t region_offset;
    size_t region_size;
    if (krffs_parse_region_args(argc, argv, (uint64_t)file_info.st_size,
                                sizeof(struct krffs_node) * 2, &region_offset, &region_size) != 0) {
        exit_status = EXIT_FAILURE;

        goto cleanup;
    }

    file_system.size = region_size;

    if ((file_system.node = krffs_map_file(file_descriptor, region_offset, file_system.size)) ==
        (void *)-1) {
        fprintf(stderr, "stageimage: cannot map '%s'.\n", argv[2]);

        file_system.node = NULL;
        exit_status = EXIT_FAILURE;

        goto cleanup;
    }

    PLATFORM_PREFIX(close(file_descriptor));
    file_descriptor = -1;

    if (file_system.node->magic != KRFFS_File_System_Magic) {
        fprintf(stderr, "stageimage: no KRFFS file system in '%s' (run mkfs.krffs first).\n",
                argv[2]);

        exit_status = EXIT_FAILURE;

        goto cleanup;
    }

    {
        struct krffs_node_time staged_time = {1, 0};

        file_system.node->uid = 0;
        file_system.node->gid = 0;
        file_system.node->access_time = staged_time;
        file_system.node->modification_time = staged_time;
        file_system.node->attribute_change_time = staged_time;
    }
    krffs_sync_mapping(file_system.node, sizeof(*file_system.node));

stage:
    manifest = fopen(argv[1], "r");
    if (manifest == NULL) {
        fprintf(stderr, "stageimage: cannot open manifest '%s'.\n", argv[1]);

        exit_status = EXIT_FAILURE;

        goto cleanup;
    }

    while (fgets(line, sizeof(line), manifest) != NULL) {
        size_t n = strlen(line);

        if (n == sizeof(line) - 1 && line[n - 1] != '\n') {
            fprintf(stderr, "stageimage: manifest line too long (over %d bytes).\n",
                    (int)sizeof(line) - 2);
            exit_status = EXIT_FAILURE;

            break;
        }
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) {
            line[--n] = '\0';
        }
        if (!apply_line(&file_system, line)) {
            exit_status = EXIT_FAILURE;

            break;
        }
    }

    if (exit_status == EXIT_SUCCESS && ferror(manifest)) {
        fprintf(stderr, "stageimage: cannot read manifest '%s'.\n", argv[1]);
        exit_status = EXIT_FAILURE;
    }
    if (fclose(manifest) != 0 && exit_status == EXIT_SUCCESS) {
        fprintf(stderr, "stageimage: cannot close manifest '%s'.\n", argv[1]);
        exit_status = EXIT_FAILURE;
    }

    if (exit_status == EXIT_SUCCESS && tree_root != NULL) {
        if (!tree_stamp(tree_root)) {
            fprintf(stderr, "stageimage: cannot set the times under '%s'.\n", tree_root);
            exit_status = EXIT_FAILURE;
        } else {
            printf("stageimage: materialised '%s' from '%s' (%lu nodes).\n", tree_root, argv[1],
                   tree_nodes);
        }
    } else if (exit_status == EXIT_SUCCESS) {
        free_after = total_free_space(&file_system);
        printf("stageimage: built '%s' from '%s' (%llu of %llu bytes used, %llu free).\n", argv[2],
               argv[1], (unsigned long long)(file_system.size - free_after),
               (unsigned long long)file_system.size, (unsigned long long)free_after);
    }

cleanup:
    if (file_descriptor != -1) {
        PLATFORM_PREFIX(close(file_descriptor));
    }

    if (file_system.node != NULL && file_system.node != (void *)-1) {
        krffs_unmap_file(file_system.node, file_system.size);
    }

    return exit_status;
}
