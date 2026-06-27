#include <dirent.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
    if (path[len - 1] == '/' ||
        strstr(path, "//") != NULL ||
        strstr(path, "/./") != NULL || strstr(path, "/../") != NULL) {
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
 * Read a whole file into a `malloc`'d buffer.
 *
 * `*len` is the byte count (no NUL is appended; the data is staged verbatim).
 * Returns NULL if the file is absent or unreadable, which lets `optional-file`
 * skip a missing host input (the Doom WAD, the SSH `authorized_keys`).
 */
static char *read_whole_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    long n;
    char *buf;

    if (!f) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    buf = (char *) malloc((size_t) n + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    if ((size_t) n != 0 && fread(buf, 1, (size_t) n, f) != (size_t) n) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    buf[n] = '\0';
    *len = (size_t) n;
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
    uint64_t h =
        (parent_id + 1) * 1099511628211ULL;

    h ^= krffs_calculate_djb_hash((uint8_t *) name);
    h *= 1099511628211ULL;

    return h ? h : 1;
}

/**
 * Find a child named `name` directly inside directory `parent_id`, or NULL.
 *
 * Mirrors the kernel's `krffs_find_child` and is what keeps staging idempotent.
 */
static struct krffs_node *find_child(
                              struct krffs_file_system *file_system,
                              uint64_t parent_id,
                              const char *name
                          )
{
    struct krffs_node *node =
        file_system->node;

    do {
        if (node->type == KRFFS_Reserved_Node &&
            node->parent_id == parent_id &&
            strcmp((const char *) node->name, name) == 0) {
            return node;
        }
    } while ((node = krffs_get_next_node(file_system, node)) != file_system->node);

    return NULL;
}

/**
 * Total free space, in bytes (the sum of every free block including headers).
 *
 * Reported at the end so a manifest author sees how much slack an image holds.
 */
static uint64_t total_free_space(struct krffs_file_system *file_system)
{
    struct krffs_node *node =
        file_system->node;
    uint64_t free_bytes =
        0;

    do {
        if (node->type == KRFFS_Free_Node) {
            free_bytes +=
                node->size;
        }
    } while ((node = krffs_get_next_node(file_system, node)) != file_system->node);

    return free_bytes;
}

/**
 * Reserve one node named `name` inside directory `parent_id`.
 *
 * The node gets `mode` and an optional `data` payload. Returns the new node's
 * id, or 0 on failure (a full image).
 */
static uint64_t stage_node(
                    struct krffs_file_system *file_system,
                    uint64_t parent_id,
                    const char *name,
                    uint16_t mode,
                    const char *data,
                    uint64_t data_size
                )
{
    struct krffs_node *node;
    struct krffs_node_time zero_time = {
        0
    };

    if (data_size > (uint64_t) SIZE_MAX) {
        fprintf(stderr, "stageimage: '%s' is too large to stage.\n", name);

        return 0;
    }

    node =
        krffs_allocate_reserved_node(
            file_system,
            data_size,
            NULL
        );

    if (!node) {
        fprintf(stderr, "stageimage: not enough space for '%s'.\n", name);

        return 0;
    }

    /* set_reserved_node_name also stamps the v1 djb id; override it with the v2
       parent-aware id below. */
    krffs_set_reserved_node_name(node, name);

    node->parent_id =
        parent_id;
    node->id =
        krffs_mix_id(parent_id, name);
    node->mode =
        mode;
    node->uid =
        0;
    node->gid =
        0;
    node->data_size =
        data_size;

    /* allocate_reserved_node does not clear the timestamps, so set them to a
       deterministic zero rather than inherit stale free-block bytes. */
    node->access_time =
        zero_time;
    node->modification_time =
        zero_time;
    node->attribute_change_time =
        zero_time;

    if (data_size > 0 && data) {
        memcpy((char *) (node + 1), data, (size_t) data_size);
    }

    krffs_sync_mapping(node, sizeof(*node) + (size_t) data_size);

    return node->id;
}

/**
 * Resolve an absolute guest directory path to its node id, creating any missing
 * component as a 0755 directory.
 *
 * The root "/" is id 0. A path like "/root/.ssh" walks "root" then ".ssh",
 * find-or-creating each as a 0755 directory; an existing component that is not a
 * directory is an error (a manifest cannot place a file under a file). A `dir`
 * directive that names the same path later sets its intended mode, so an explicit
 * mode wins whether declared before or after a file places something inside.
 * Returns the final directory id (0 for the root "/"), or (uint64_t)-1 on a
 * malformed path or a non-directory component.
 */
static uint64_t resolve_dir(struct krffs_file_system *file_system, const char *path)
{
    char buf[256];
    uint64_t parent =
        0;
    char *p;
    char *save = NULL;

    if (path[0] != '/') {
        fprintf(stderr, "stageimage: directory path '%s' is not absolute.\n", path);

        return (uint64_t) -1;
    }
    if (path[1] == '\0') {
        return 0;
    }
    if (strlen(path) >= sizeof(buf)) {
        fprintf(stderr, "stageimage: path '%s' is too long.\n", path);

        return (uint64_t) -1;
    }
    strcpy(buf, path + 1);

    for (p = strtok_r(buf, "/", &save); p != NULL; p = strtok_r(NULL, "/", &save)) {
        struct krffs_node *existing =
            find_child(file_system, parent, p);
        uint64_t id;

        if (existing != NULL) {
            if (!is_dir_node(existing)) {
                fprintf(stderr,
                        "stageimage: path '%s' descends through non-directory '%s'.\n",
                        path, p);

                return (uint64_t) -1;
            }
            id =
                existing->id;
        } else {
            id =
                stage_node(file_system, parent, p,
                           (uint16_t) (KRFFS_MODE_IFDIR | 0755), NULL, 0);
            if (id == 0) {
                return (uint64_t) -1;
            }
        }
        parent =
            id;
    }

    return parent;
}

/**
 * Split an absolute guest path into its parent directory and basename.
 *
 * `dir_out` receives the parent path ("/" for a root-level name); the returned
 * pointer is the basename inside `path_buf` (which is modified in place). Returns
 * NULL on a malformed path (no slash, trailing slash, too long).
 */
static char *split_path(const char *path, char *path_buf, size_t bufsz, char *dir_out, size_t dirsz)
{
    char *slash;

    if (path[0] != '/' || strlen(path) >= bufsz) {
        return NULL;
    }
    strcpy(path_buf, path);
    slash =
        strrchr(path_buf, '/');
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
        size_t dlen =
            (size_t) (slash - path_buf);

        if (dlen >= dirsz) {
            return NULL;
        }
        memcpy(dir_out, path_buf, dlen);
        dir_out[dlen] =
            '\0';
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
 * Stage a directory `path` with `mode`, creating any missing parent as 0755.
 *
 * Unlike the implicit 0755 directories that `resolve_dir` makes for a file's
 * parents, this sets an explicit mode, and updates the mode if the directory
 * already exists (so a `dir 0700 /etc/dropbear` line wins whether it is declared
 * before or after something is placed inside). Returns false on any failure.
 */
static bool stage_dir(struct krffs_file_system *file_system, const char *path, uint16_t mode)
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

    name =
        split_path(path, path_buf, sizeof(path_buf), dir_buf, sizeof(dir_buf));
    if (name == NULL) {
        fprintf(stderr, "stageimage: malformed directory path '%s'.\n", path);

        return false;
    }

    parent_id =
        resolve_dir(file_system, dir_buf);
    if (parent_id == (uint64_t) -1) {
        return false;
    }

    node =
        find_child(file_system, parent_id, name);
    if (node != NULL) {
        if (!is_dir_node(node)) {
            fprintf(stderr, "stageimage: '%s' already exists as a non-directory.\n", path);

            return false;
        }
        node->mode =
            (uint16_t) (KRFFS_MODE_IFDIR | mode);
        krffs_sync_mapping(node, sizeof(*node));

        return true;
    }

    return stage_node(file_system, parent_id, name,
                      (uint16_t) (KRFFS_MODE_IFDIR | mode), NULL, 0) != 0;
}

/**
 * Stage a file `path` with `mode` and the given payload, creating its parent
 * directories as needed. A path under /dev or /proc is rejected. Returns false
 * on any failure.
 */
static bool stage_file(
                struct krffs_file_system *file_system,
                const char *path,
                uint16_t mode,
                const char *data,
                uint64_t data_size
            )
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

    name =
        split_path(path, path_buf, sizeof(path_buf), dir_buf, sizeof(dir_buf));
    if (name == NULL) {
        fprintf(stderr, "stageimage: malformed file path '%s'.\n", path);

        return false;
    }

    dir_id =
        resolve_dir(file_system, dir_buf);
    if (dir_id == (uint64_t) -1) {
        return false;
    }

    /* Each image is built fresh, so a name that already exists means the manifest
       stages the same path twice; that is an authoring error, not an update. */
    if (find_child(file_system, dir_id, name) != NULL) {
        fprintf(stderr, "stageimage: '%s' is staged more than once.\n", path);

        return false;
    }

    return stage_node(file_system, dir_id, name, mode, data, data_size) != 0;
}

/**
 * Parse an octal mode field (e.g. "0755"). Returns false on a non-octal token.
 */
static bool parse_mode(const char *s, uint16_t *out)
{
    char *end;
    unsigned long v =
        strtoul(s, &end, 8);

    if (*s == '\0' || *end != '\0' || v > 07777u) {
        return false;
    }
    *out =
        (uint16_t) v;

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
    char *r =
        s;
    char *w =
        s;

    while (*r) {
        if (*r == '\\' && r[1]) {
            r++;
            switch (*r) {
            case 'n': *w++ = '\n'; break;
            case 't': *w++ = '\t'; break;
            case '\\': *w++ = '\\'; break;
            default: *w++ = '\\'; *w++ = *r; break;
            }
            r++;
        } else {
            *w++ = *r++;
        }
    }
    *w =
        '\0';

    return (size_t) (w - s);
}

/**
 * Stage every regular, executable file directly inside host directory `hostdir`
 * into guest directory `guestdir` with `mode`.
 *
 * This stages a whole built set (the test probes in build/tests, the utils in
 * build/utils) without naming each. It is NOT recursive and skips anything that is
 * not a regular executable (via `lstat`, so a symlink is left out too), which is
 * why the GCC-written `.d` dependency files beside the binaries are not staged.
 * Returns false on a host or staging error.
 */
static bool stage_exec_dir(
                struct krffs_file_system *file_system,
                const char *guestdir,
                const char *hostdir,
                uint16_t mode
            )
{
    DIR *d =
        opendir(hostdir);
    struct dirent *e;
    bool ok =
        true;

    if (d == NULL) {
        fprintf(stderr, "stageimage: cannot open host dir '%s'.\n", hostdir);

        return false;
    }

    while (ok && (e = readdir(d)) != NULL) {
        char hostpath[1024];
        char guestpath[512];
        struct stat st;
        char *data;
        size_t len = 0;
        int n;

        if (e->d_name[0] == '.') {
            continue;
        }
        n =
            snprintf(hostpath, sizeof(hostpath), "%s/%s", hostdir, e->d_name);
        if (n < 0 || (size_t) n >= sizeof(hostpath)) {
            fprintf(stderr, "stageimage: host path under '%s' is too long.\n", hostdir);
            ok =
                false;
            break;
        }
        /* lstat, not stat, so a symlink is left out: only a real regular file with
           an execute bit (the built binaries are 0755) is staged. */
        if (lstat(hostpath, &st) != 0 || !S_ISREG(st.st_mode) ||
            !(st.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH))) {
            continue;
        }

        data =
            read_whole_file(hostpath, &len);
        if (data == NULL) {
            fprintf(stderr, "stageimage: cannot read '%s'.\n", hostpath);
            ok =
                false;
            break;
        }

        if (strcmp(guestdir, "/") == 0) {
            n = snprintf(guestpath, sizeof(guestpath), "/%s", e->d_name);
        } else {
            n = snprintf(guestpath, sizeof(guestpath), "%s/%s", guestdir, e->d_name);
        }
        if (n < 0 || (size_t) n >= sizeof(guestpath)) {
            fprintf(stderr, "stageimage: guest path under '%s' is too long.\n", guestdir);
            free(data);
            ok =
                false;
            break;
        }
        ok =
            stage_file(file_system, guestpath, mode, data, len);
        free(data);
    }

    closedir(d);

    return ok;
}

/**
 * Apply one manifest line. Returns false on a hard error (a syntax error or a
 * staging failure), true on success or a skipped `optional-file`.
 *
 * Directives (the leading token), with octal MODE and absolute guest paths:
 *   dir   MODE PATH               a directory
 *   file  MODE PATH HOSTPATH      a file copied from the host
 *   optional-file MODE PATH HOST  like file, but skipped if HOST is absent
 *   text  MODE PATH TEXT          a file with inline text (\n, \t, \\ honored)
 *   exec-dir MODE GUESTDIR HOST   every executable file in HOST, into GUESTDIR
 */
static bool apply_line(struct krffs_file_system *file_system, char *line)
{
    char *save = NULL;
    char *verb =
        strtok_r(line, " \t", &save);
    char *mode_s;
    char *path;
    uint16_t mode;

    if (verb == NULL || verb[0] == '#') {
        return true;
    }

    mode_s =
        strtok_r(NULL, " \t", &save);
    path =
        strtok_r(NULL, " \t", &save);
    if (mode_s == NULL || path == NULL || !parse_mode(mode_s, &mode)) {
        fprintf(stderr, "stageimage: malformed '%s' directive.\n", verb);

        return false;
    }

    if (strcmp(verb, "dir") == 0) {
        return stage_dir(file_system, path, mode);
    }
    if (strcmp(verb, "text") == 0) {
        char *text =
            strtok_r(NULL, "", &save);
        size_t len;

        if (text == NULL) {
            text =
                (char *) "";
        }
        while (*text == ' ' || *text == '\t') {
            text++;
        }
        len =
            unescape_text(text);

        return stage_file(file_system, path, mode, text, len);
    }
    if (strcmp(verb, "file") == 0 || strcmp(verb, "optional-file") == 0) {
        char *hostpath =
            strtok_r(NULL, " \t", &save);
        char *data;
        size_t len = 0;
        bool ok;

        if (hostpath == NULL) {
            fprintf(stderr, "stageimage: '%s' needs a host path.\n", verb);

            return false;
        }
        /* optional-file skips ONLY a genuinely absent host file. A file that exists
           but cannot be read (a permission error, a short read) is a real error for
           both directives, never a silent skip. */
        if (access(hostpath, F_OK) != 0) {
            if (strcmp(verb, "optional-file") == 0) {
                printf("stageimage: optional '%s' absent (%s); skipping.\n", path, hostpath);

                return true;
            }
            fprintf(stderr, "stageimage: '%s' does not exist.\n", hostpath);

            return false;
        }
        data =
            read_whole_file(hostpath, &len);
        if (data == NULL) {
            fprintf(stderr, "stageimage: cannot read '%s'.\n", hostpath);

            return false;
        }
        ok =
            stage_file(file_system, path, mode, data, len);
        free(data);

        return ok;
    }
    if (strcmp(verb, "exec-dir") == 0) {
        char *hostdir =
            strtok_r(NULL, " \t", &save);

        if (hostdir == NULL) {
            fprintf(stderr, "stageimage: 'exec-dir' needs a host dir.\n");

            return false;
        }

        return stage_exec_dir(file_system, path, hostdir, mode);
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
    int exit_status =
        EXIT_SUCCESS;
    int file_descriptor =
        -1;
    struct krffs_file_system file_system = {
        .node = NULL
    };
    struct stat file_info;
    FILE *manifest;
    char line[2048];
    uint64_t free_after;

    if (argc < 3 || argv[1][0] == '-') {
        fprintf(
            stderr,
            "Usage: %s <manifest> <image>\n",
            argc >= 1 ? argv[0] : "stageimage.krffs"
        );

        return EXIT_FAILURE;
    }

    if ((file_descriptor = PLATFORM_PREFIX(open(argv[2], O_RDWR))) == -1) {
        fprintf(stderr, "stageimage: cannot open '%s'.\n", argv[2]);

        return EXIT_FAILURE;
    }

    if (fstat(file_descriptor, &file_info) == -1 || !S_ISREG(file_info.st_mode)) {
        fprintf(stderr, "stageimage: '%s' is not a regular file.\n", argv[2]);

        exit_status =
            EXIT_FAILURE;

        goto cleanup;
    }

    if (file_info.st_size < (off_t) (sizeof(struct krffs_node) * 2)) {
        fprintf(stderr, "stageimage: '%s' is too small to hold a file system.\n", argv[2]);

        exit_status =
            EXIT_FAILURE;

        goto cleanup;
    }

    file_system.size =
        file_info.st_size;

    if ((file_system.node =
             krffs_map_file(
                 file_descriptor,
                 0,
                 file_system.size
             )) == (void *) -1) {
        fprintf(stderr, "stageimage: cannot map '%s'.\n", argv[2]);

        file_system.node =
            NULL;
        exit_status =
            EXIT_FAILURE;

        goto cleanup;
    }

    PLATFORM_PREFIX(close(file_descriptor));
    file_descriptor =
        -1;

    if (file_system.node->magic != KRFFS_File_System_Magic) {
        fprintf(stderr, "stageimage: no KRFFS file system in '%s' (run mkfs.krffs first).\n", argv[2]);

        exit_status =
            EXIT_FAILURE;

        goto cleanup;
    }

    manifest =
        fopen(argv[1], "r");
    if (manifest == NULL) {
        fprintf(stderr, "stageimage: cannot open manifest '%s'.\n", argv[1]);

        exit_status =
            EXIT_FAILURE;

        goto cleanup;
    }

    while (fgets(line, sizeof(line), manifest) != NULL) {
        size_t n =
            strlen(line);

        /* A line that exactly fills the buffer with no newline was truncated by
           fgets; staging its prefix and reading the rest as a bogus directive would
           both be wrong, so reject it. */
        if (n == sizeof(line) - 1 && line[n - 1] != '\n') {
            fprintf(stderr, "stageimage: manifest line too long (over %d bytes).\n",
                    (int) sizeof(line) - 2);
            exit_status =
                EXIT_FAILURE;

            break;
        }
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) {
            line[--n] =
                '\0';
        }
        if (!apply_line(&file_system, line)) {
            exit_status =
                EXIT_FAILURE;

            break;
        }
    }

    fclose(manifest);

    if (exit_status == EXIT_SUCCESS) {
        free_after =
            total_free_space(&file_system);
        printf(
            "stageimage: built '%s' from '%s' (%llu of %llu bytes used, %llu free).\n",
            argv[2], argv[1],
            (unsigned long long) (file_system.size - free_after),
            (unsigned long long) file_system.size,
            (unsigned long long) free_after
        );
    }

cleanup:
    if (file_descriptor != -1) {
        PLATFORM_PREFIX(close(file_descriptor));
    }

    if (file_system.node != NULL && file_system.node != (void *) -1) {
        krffs_unmap_file(file_system.node, file_system.size);
    }

    return exit_status;
}
