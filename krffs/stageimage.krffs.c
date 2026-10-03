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

/* The KRFFS v2 directory type bit (mode & S_IFMT == S_IFDIR marks a directory).
   The host stat.h S_IFDIR may differ, so the on-disk value is spelled out. */
#define KRFFS_MODE_IFDIR 0040000u
#define KRFFS_MODE_IFMT  0170000u

/**
 * True if `node`'s mode marks it a KRFFS directory.
 */
static bool is_dir_node(const struct krffs_node *node)
{
    return (node->mode & KRFFS_MODE_IFMT) == KRFFS_MODE_IFDIR;
}

/**
 * Reject a guest path that is not clean and absolute.
 *
 * A clean path is absolute, has no empty component (a doubled slash would
 * create one, letting a doubled-slash form of /dev slip past the synthetic
 * namespace check), no `.` or `..` component, and no trailing slash (except
 * the root "/"). The manifest names every path literally, so anything that is
 * not clean is an authoring error.
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
 * The host path a manifest's `path` is read from.
 *
 * A manifest names its host files by their i686 paths, build/... and
 * vendor/musl/install/..., so one manifest describes the image of either
 * architecture. The root Makefile of an x86-64 build sets RACH2_PATH_MAP to
 * blank-separated `FROM=TO` pairs, and the first FROM that begins `path` is
 * replaced by its TO: build/utils/cat is then read from
 * build/x86_64/utils/cat. With no map, or no FROM that matches, the path is
 * read as it is written. The result is in one of four static buffers, so it
 * is good until the fourth call after this one.
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
 * Record one host input path for the build's dependency tracking.
 *
 * When `STAGEIMAGE_DEPS` names a file, every host path the stager reads or
 * walks is appended there, one per line, absent `optional-file` paths
 * included. The build turns the list into make dependencies, so a changed,
 * added, or removed input restages exactly the images that carry it.
 */
static void record_input(const char *path)
{
    static FILE *deps_file;
    static int deps_tried;

    if (!deps_tried) {
        const char *name = getenv("STAGEIMAGE_DEPS");

        deps_tried = 1;
        if (name != NULL && *name != '\0') {
            deps_file = fopen(name, "w");
        }
    }
    if (deps_file != NULL) {
        fprintf(deps_file, "%s\n", path);
    }
}

/**
 * Read a whole file into a `malloc`'d buffer.
 *
 * `*len` is the byte count (no NUL is appended; the data is staged verbatim).
 * Returns NULL if the file is absent or unreadable, which lets `optional-file`
 * skip a missing host input (the Doom WAD, the SSH `authorized_keys`).
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
 * Mix a parent id and a node name into a stable node id.
 *
 * This matches the kernel's `krffs_mix_id` (rach2/sys/fs/krffs.c) so a
 * host-staged node and the same node created in-guest get the same id.
 */
static uint64_t krffs_mix_id(uint64_t parent_id, const char *name)
{
    uint64_t h = (parent_id + 1) * 1099511628211ULL;

    h ^= krffs_calculate_djb_hash((uint8_t *)name);
    h *= 1099511628211ULL;

    return h ? h : 1;
}

/**
 * Find a child named `name` directly inside directory `parent_id`, or NULL.
 *
 * Mirrors the kernel's `krffs_find_child` and is what keeps staging idempotent.
 */
static struct krffs_node *find_child(struct krffs_file_system *file_system, uint64_t parent_id,
                                     const char *name)
{
    struct krffs_node *node = file_system->node;

    do {
        if (node->type == KRFFS_Reserved_Node && node->parent_id == parent_id &&
            strcmp((const char *)node->name, name) == 0) {
            return node;
        }
    } while ((node = krffs_get_next_node(file_system, node)) != file_system->node);

    return NULL;
}

/**
 * Find a staged directory whose id is `id`, or NULL.
 *
 * Mirrors the kernel's `krffs_id_taken`: a reserved node with a directory's
 * mode and a name, since a node with no name is an orphan and no path reaches
 * it.
 */
static struct krffs_node *find_dir_by_id(struct krffs_file_system *file_system, uint64_t id)
{
    struct krffs_node *node = file_system->node;

    do {
        if (node->type == KRFFS_Reserved_Node && is_dir_node(node) && node->name[0] != '\0' &&
            node->id == id) {
            return node;
        }
    } while ((node = krffs_get_next_node(file_system, node)) != file_system->node);

    return NULL;
}

/**
 * Write the guest path of the staged directory `node` into `buf`, for a
 * message.
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
        node = node->parent_id == 0 ? NULL : find_dir_by_id(file_system, node->parent_id);
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
 * Refuse to stage the directory `path`, named `name` in `parent_id`, when a
 * staged directory already has the id the two mix to. Returns true when the
 * id is free.
 *
 * Two names can mix to one id: `bC` and `cb` have one DJB hash. A child
 * records its parent by id alone, so a second directory with a taken id would
 * share its children with the first. The kernel refuses to make one
 * (`krffs_id_taken` in rach2/sys/fs/krffs.c), and an image with one would give
 * it a hierarchy the kernel never builds, so the stager refuses too.
 */
static bool dir_id_free(struct krffs_file_system *file_system, uint64_t parent_id, const char *name,
                        const char *path)
{
    const struct krffs_node *other = find_dir_by_id(file_system, krffs_mix_id(parent_id, name));
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
 * Total free space, in bytes (the sum of every free block including headers).
 *
 * Reported at the end so a manifest author sees how much slack an image has.
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
 * Reserve one node named `name` inside directory `parent_id`.
 *
 * The node gets `mode`, the owner `uid`:`gid` (0:0 unless the manifest says
 * otherwise), and an optional `data` payload. Returns the new node's id, or 0
 * on failure (a full image).
 */
static uint64_t stage_node(struct krffs_file_system *file_system, uint64_t parent_id,
                           const char *name, uint16_t mode, uint32_t uid, uint32_t gid,
                           const char *data, uint64_t data_size)
{
    struct krffs_node *node;
    /* Every staged node has this one fixed instant: one second past the
       epoch. Constant, because an image built twice from one tree must be one
       image; nonzero, because pdpmake reads a modification time of zero as a
       file that does not exist, and a student must be able to run make over
       the staged sources in place. Anything the machine builds is stamped by
       its real clock and is therefore newer than any staged source. */
    struct krffs_node_time staged_time = {1, 0};

    if (data_size > (uint64_t)SIZE_MAX) {
        fprintf(stderr, "stageimage: '%s' is too large to stage.\n", name);

        return 0;
    }

    node = krffs_allocate_reserved_node(file_system, data_size, NULL);

    if (!node) {
        fprintf(stderr, "stageimage: not enough space for '%s'.\n", name);

        return 0;
    }

    /* set_reserved_node_name also stamps the v1 djb id; override it with the v2
       parent-aware id below. */
    krffs_set_reserved_node_name(node, name);

    node->parent_id = parent_id;
    node->id = krffs_mix_id(parent_id, name);
    node->mode = mode;
    node->uid = uid;
    node->gid = gid;
    node->data_size = data_size;

    /* allocate_reserved_node does not clear the timestamps, so set them to
       the fixed staged instant, not inherit stale free-block bytes. */
    node->access_time = staged_time;
    node->modification_time = staged_time;
    node->attribute_change_time = staged_time;

    if (data_size > 0 && data) {
        memcpy((char *)(node + 1), data, (size_t)data_size);
    }

    krffs_sync_mapping(node, sizeof(*node) + (size_t)data_size);

    return node->id;
}

/**
 * Resolve an absolute guest directory path to its node id, creating any missing
 * component as a 0755 directory.
 *
 * The root "/" is id 0. A path like "/root/.ssh" walks "root" then ".ssh",
 * find-or-creating each as a 0755 directory; an existing component that is
 * not a directory is an error (a manifest cannot place a file under a file).
 * A `dir` directive that names the same path later sets its intended mode,
 * so an explicit mode wins whether declared before or after a file places
 * something inside. Returns the final directory id (0 for the root "/"), or
 * (uint64_t)-1 on a malformed path or a non-directory component.
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
        struct krffs_node *existing = find_child(file_system, parent, p);
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

            snprintf(here, sizeof(here), "%.*s", (int)(1 + (p - buf) + strlen(p)), path);
            if (!dir_id_free(file_system, parent, p, here)) {
                return (uint64_t)-1;
            }
            id = stage_node(file_system, parent, p, (uint16_t)(KRFFS_MODE_IFDIR | 0755), 0, 0, NULL,
                            0);
            if (id == 0) {
                return (uint64_t)-1;
            }
        }
        parent = id;
    }

    return parent;
}

/**
 * Split an absolute guest path into its parent directory and basename.
 *
 * `dir_out` receives the parent path ("/" for a root-level name); the
 * returned pointer is the basename inside `path_buf` (which is modified in
 * place). Returns NULL on a malformed path (no slash, trailing slash, too
 * long).
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
        /* Root-level name: parent is "/". */
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
 * Reject /dev and /proc paths.
 *
 * Those are kernel-synthesized namespaces (devfs and procfs), never backed by
 * KRFFS nodes, so a manifest must not stage anything there.
 */
static bool is_synthetic_path(const char *path)
{
    return strcmp(path, "/dev") == 0 || strncmp(path, "/dev/", 5) == 0 ||
           strcmp(path, "/proc") == 0 || strncmp(path, "/proc/", 6) == 0;
}

/**
 * Stage a directory `path` with `mode` and owner, creating any missing parent
 * as root-owned 0755.
 *
 * Unlike the implicit 0755 directories that `resolve_dir` makes for a file's
 * parents, this sets an explicit mode and owner, and updates both if the
 * directory already exists (so a `dir 0700:1000:1000 /home/test` line wins
 * whether it is declared before or after something is placed inside). Returns
 * false on any failure.
 */
/*
 * The tree mode, `stageimage MANIFEST --tree DIR`: the manifest is
 * materialised into a host directory instead of an image, one host file or
 * directory per node, and one line `node MODE UID GID PATH` is printed on
 * standard output for each, with the POSIX type bits in the mode.
 * tools/image/mkext2.sh hands the directory to mke2fs and the lines to
 * debugfs, which sets the owners and the set-id bits a build that is not
 * root cannot put on a host file. Every node gets the fixed staged instant
 * as its times, one second after the epoch, so that one manifest makes one
 * image.
 */
static const char *tree_root;
static unsigned long tree_nodes;

static void tree_note(uint16_t mode, uint32_t uid, uint32_t gid, const char *path)
{
    printf("node 0%o %lu %lu %s\n", (unsigned)mode, (unsigned long)uid, (unsigned long)gid, path);
    tree_nodes++;
}

/* Make `host_path` and its missing parents. A directory the manifest did
 * not name but a path passes through is a node too, root's and 0755, and is
 * noted so that the image does not keep the builder's identity and umask
 * on it; the last component is noted when `note_last`, which a caller
 * naming the directory itself leaves to its own note. */
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
    /* A name already there is the same path staged twice, as in an image;
       the exclusive create decides that in one step, and follows no link. */
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

/* The fixed instant on every node, directories last, since writing a file
 * moves its directory's time. */
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

    node = find_child(file_system, parent_id, name);
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
                      NULL, 0) != 0;
}

/**
 * Stage a file `path` with `mode` and the given payload, creating its parent
 * directories as needed. A path under /dev or /proc is rejected. Returns false
 * on any failure.
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

    /* Each image is built fresh, so a name that already exists means the
     * manifest stages the same path twice; that is an authoring error, not an
     * update. */
    if (find_child(file_system, dir_id, name) != NULL) {
        fprintf(stderr, "stageimage: '%s' is staged more than once.\n", path);

        return false;
    }

    return stage_node(file_system, dir_id, name, mode, uid, gid, data, data_size) != 0;
}

/**
 * Parse a mode field: octal permissions with an optional owner, `0755` or
 * `0700:1000:1000`. The owner defaults to root (0:0), so every existing
 * manifest reads unchanged. Returns false on a malformed token.
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
 * Unescape a manifest `text` payload in place, honoring \n, \t, and \\.
 *
 * Returns the new length. The manifest is line-oriented, so a multi-line file
 * (an /etc/hosts, a grub.cfg) is written on one `text` line with \n escapes.
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

/** Order two directory entries by name, for the `qsort` in both stagers. */
static int compare_names(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/**
 * Stage every regular, executable file directly inside host directory `hostdir`
 * into guest directory `guestdir` with `mode`.
 *
 * This stages a whole built set (the test probes in build/tests, the utils
 * in build/utils) without naming each. It is NOT recursive and skips
 * anything that is not a regular executable (via `lstat`, so a symlink is
 * left out too), which is why the GCC-written `.d` dependency files beside
 * the binaries are not staged. The names are read first and staged in sorted
 * order, so one host directory always produces one image: `readdir` order is
 * the file system's, and on APFS it is a hash of the name. Returns false on
 * a host or staging error.
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

    record_input(hostdir);
    d = opendir(hostdir);
    if (d == NULL) {
        fprintf(stderr, "stageimage: cannot open host dir '%s'.\n", hostdir);

        return false;
    }

    /* The names are read first and sorted, the way `stage_tree` already does
       it: readdir order is the host file system's, it differs between machines
       and between two builds on one machine, and an image built twice from one
       tree should be one image. */
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
    /* readdir returns NULL for the end AND for an error, so a directory read
       part way would otherwise be staged as if it were whole. */
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
        /* lstat, not stat, so a symlink is left out: only a real regular file
         * with an execute bit (the built binaries are 0755) is staged. */
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
 * Make `path` a directory if it is not one already, leaving an existing one
 * exactly as it is. Returns false on a hard error.
 *
 * `stage_dir` re-applies mode and owner to a directory it finds, which is what
 * a `dir` line should do: the manifest said what that directory is. A `tree`
 * must not: it walks INTO directories that other lines declared, and a tree
 * owned by uid 1000 staged under a root-owned 0700 directory would otherwise
 * hand that directory away.
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

            return true; /* somebody else declared it; it is theirs */
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
    node = find_child(file_system, parent_id, name);
    if (node != NULL) {
        if (!is_dir_node(node)) {
            fprintf(stderr, "stageimage: '%s' already exists as a non-directory.\n", path);

            return false;
        }

        return true; /* somebody else declared it; it is theirs */
    }

    return stage_dir(file_system, path, mode, uid, gid);
}

/**
 * Stage every regular file under `hostdir` into `guestdir`, recursively.
 *
 * `exec-dir` above stages the executables of ONE host directory and is what a
 * tree of build products wants. This is for a tree of SOURCE: the ported musl
 * ships 218 headers across eight directories, and a manifest naming each of
 * them would be regenerated by hand every time the library moved.
 *
 * The semantics are fixed, not inferred, because a stager that guesses
 * produces an image nobody can reason about:
 *
 *   * `mode` is the mode of every FILE. Directories this walk creates get 0755
 *     with the same owner, since a directory needs its execute bit to be walked
 *     and a header does not.
 *   * Only regular files are staged. A symlink, device or socket under the host
 *     directory is an ERROR and not a skip: KRFFS has no symlinks, and quietly
 *     dropping one would make the image differ from the tree it was staged from
 *     without saying so.
 *   * A zero-byte file is staged as a zero-byte file. musl ships five, and an
 *     image missing them is one whose `#include` fails for no visible reason.
 *   * The walk is sorted, so one host tree always produces one image.
 *   * A dotfile is skipped, as `exec-dir` skips them.
 *
 * Returns false on any error.
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

    /* The host directory itself must be a directory and not a link to one:
       everything below is checked with lstat, and a symlinked root would walk
       a tree the manifest did not name. */
    if (lstat(hostdir, &root_st) != 0 || !S_ISDIR(root_st.st_mode)) {
        fprintf(stderr,
                "stageimage: '%s' is not a directory; 'tree' takes "
                "one, and follows no symlink to reach it.\n",
                hostdir);
        closedir(d);

        return false;
    }

    /* Read the names first, then sort them: readdir order is the file system's
       and differs between machines, and an image should not. */
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
    /* readdir returns NULL for the end AND for an error, and telling them apart
       is the difference between a complete tree and a prefix of one published
       as if it were complete. */
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
 * Apply one manifest line. Returns false on a hard error (a syntax error or a
 * staging failure), true on success or a skipped `optional-file`.
 *
 * Directives (the leading token), with MODE an octal permission set carrying
 * an optional owner (`0755` or `0700:1000:1000`, root-owned by default) and
 * absolute guest paths:
 *   dir   MODE PATH               a directory
 *   file  MODE PATH HOSTPATH      a file copied from the host
 *   optional-file MODE PATH HOST  like file, but skipped if HOST is absent
 *   text  MODE PATH TEXT          a file with inline text (\n, \t, \\ honored)
 *   exec-dir MODE GUESTDIR HOST   every executable file in HOST, into GUESTDIR
 *   tree     MODE GUESTDIR HOST   every regular file under HOST, recursively
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
        /* optional-file skips ONLY a genuinely absent host file. A file that
           exists but cannot be read (a permission error, a short read) is a
           real error for both directives, never a silent skip. The absence
           is recorded before the skip: the input inventory must name the
           path so its later appearance restages the image. */
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
        /* A set-id mode is refused for `exec-dir`'s reason doubled: the set of
           paths it would privilege is a property of what happens to be in the
           host tree, and a tree is deeper than one directory. */
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
 * Build a KRFFS image from a manifest.
 *
 * Reads the line-oriented manifest and stages each entry directly into an
 * already-`mkfs`'d image as v2 nodes (the krffs FUSE mount cannot mkdir, so the
 * whole tree, directories and files alike, is written here). Staging is
 * find-or-create, so a re-run completes a partial image. On a full image the
 * failing node is reported and the build fails; the caller raises the size.
 *
 * Usage: `stageimage.krffs <manifest> <image>`. Host paths in the manifest are
 * resolved against the current directory, so the build runs it from the repo
 * root where `build/...` resolves.
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
        /* A fresh directory, so that nothing a previous run left can end up
           in an image; mke2fs takes whatever is there. */
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

    /* mkfs stamps the root node with the building host's ids and the building
       host's CLOCK (the course tool's contract). A kernel-bound image boots
       multi-user, so its root directory belongs to root whoever built it; and
       its times are the fixed staged instant for the same reason every staged
       node's are, which is that an image built twice from one tree must be
       one image. Without this the root node alone carried the moment of the
       build, and two clean builds of the same commit differed in those
       twenty-four bytes. The value is the same instant every staged node gets
       (see stage_file). */
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

        /* A line that exactly fills the buffer with no newline was truncated by
           fgets; staging its prefix and reading the rest as a bogus directive
           would both be wrong, so reject it. */
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

    /* An end that was not the end of the file is a read that failed. */
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
