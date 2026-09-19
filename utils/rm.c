/** Remove files; with `-r`, remove directories and their contents. */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>

#define MAX_PATH 256

static int remove_recursive(const char *path);

/**
 * Removes every entry inside the directory `dir_path` (but not `dir_path`).
 *
 * Walks the directory, skips `.` and `..` and any child whose full path would
 * reach `MAX_PATH`, and recurses into each with `remove_recursive`. Returns -1
 * if the directory cannot be opened or any child removal fails.
 */
static int remove_dir_contents(const char *dir_path)
{
    DIR *dir = opendir(dir_path);
    struct dirent *de;
    char full[MAX_PATH];
    size_t dir_len;
    int result = 0;

    if (!dir) {
        return -1;
    }
    dir_len = strlen(dir_path);

    while ((de = readdir(dir)) != NULL) {
        size_t name_len;
        size_t pos;

        if (de->d_name[0] == '.' &&
            (de->d_name[1] == '\0' || (de->d_name[1] == '.' && de->d_name[2] == '\0'))) {
            continue;
        }

        name_len = strlen(de->d_name);
        if (dir_len + 1 + name_len >= MAX_PATH) {
            continue;
        }

        memcpy(full, dir_path, dir_len);
        pos = dir_len;
        if (dir_len > 0 && dir_path[dir_len - 1] != '/') {
            full[pos++] = '/';
        }
        memcpy(full + pos, de->d_name, name_len + 1);

        if (remove_recursive(full) < 0) {
            result = -1;
        }
    }

    closedir(dir);
    return result;
}

/**
 * Remove `path`, recursing into it if it is a non-empty directory.
 *
 * Tries `unlink` first, which removes a plain file in one step; on failure it
 * falls back to emptying the directory and then `rmdir`.
 */
static int remove_recursive(const char *path)
{
    if (unlink(path) == 0) {
        return 0;
    }
    if (remove_dir_contents(path) < 0) {
        return -1;
    }
    return rmdir(path);
}

int main(int argc, char *argv[])
{
    int i;
    int recursive = 0;
    int start = 1;
    int failed = 0;

    if (argc < 2) {
        fputs("Usage: rm [-r] <file> [...]\n", stderr);
        return 1;
    }

    if (argv[1][0] == '-' && argv[1][1] == 'r' && argv[1][2] == '\0') {
        recursive = 1;
        start = 2;
    }

    if (start >= argc) {
        fputs("Usage: rm [-r] <file> [...]\n", stderr);
        return 1;
    }

    for (i = start; i < argc; ++i) {
        int result = recursive ? remove_recursive(argv[i]) : unlink(argv[i]);
        if (result < 0) {
            fprintf(stderr, "rm: failed to remove '%s'\n", argv[i]);
            failed = 1;
        }
    }

    return failed;
}
