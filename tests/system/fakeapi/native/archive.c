/* archive.c: read prepared ustar assets without a host archive command. */
#include "api.h"
#include "../../../../tools/etude/volume/volume.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
int unpack_assets(const char *archive, const char *destination)
{
    struct buffer tests = {0}, tools = {0}, vendor = {0};
    char why[512];
    int status;
    const char *map[7];
    putsb(&tests, destination);
    putsb(&tests, "/tests");
    putsb(&tools, destination);
    putsb(&tools, "/tools");
    putsb(&vendor, destination);
    putsb(&vendor, "/vendor");
    map[0] = "tests";
    map[1] = tests.data;
    map[2] = "tools";
    map[3] = tools.data;
    map[4] = "vendor";
    map[5] = vendor.data;
    map[6] = NULL;
    status = etude_volume_read(archive, map, why, sizeof(why));
    if (status)
        log_line("assets: %s", why);
    clear(&tests);
    clear(&tools);
    clear(&vendor);
    return status ? 2 : 0;
}
int remove_assets(const char *path)
{
    struct stat st;
    DIR *dir;
    struct dirent *entry;
    if (lstat(path, &st))
        return errno == ENOENT ? 0 : 1;
    if (!S_ISDIR(st.st_mode))
        return unlink(path) != 0;
    dir = opendir(path);
    if (!dir)
        return 1;
    while ((entry = readdir(dir))) {
        struct buffer child = {0};
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        putsb(&child, path);
        putsb(&child, "/");
        putsb(&child, entry->d_name);
        if (remove_assets(child.data)) {
            clear(&child);
            closedir(dir);
            return 1;
        }
        clear(&child);
    }
    closedir(dir);
    return rmdir(path) != 0;
}
