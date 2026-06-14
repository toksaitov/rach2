/*
 * ls.c - list a directory (one entry per line, type flag + name).
 *
 * Ported to musl: <dirent.h> opendir/readdir (replacing the hand-rolled
 * getdents64 loop), no int-0x80 stubs. Dotfiles are hidden, as before.
 */

#include <stdio.h>
#include <dirent.h>

static void print_entry(const char *name, unsigned char d_type)
{
    char type;

    switch (d_type) {
        case DT_DIR: type = 'd'; break;
        case DT_REG: type = '-'; break;
        case DT_LNK: type = 'l'; break;
        default:     type = '?'; break;
    }
    /* No stat() detail; show the type flag and a fixed rw- placeholder. */
    printf("%crw- %s\n", type, name);
}

int main(int argc, char *argv[])
{
    const char *dir_path = (argc > 1) ? argv[1] : ".";
    DIR *dir;
    struct dirent *de;

    dir = opendir(dir_path);
    if (dir == NULL) {
        fputs("ls: cannot open directory\n", stderr);
        return 1;
    }

    while ((de = readdir(dir)) != NULL) {
        if (de->d_name[0] != '.') {
            print_entry(de->d_name, de->d_type);
        }
    }

    closedir(dir);
    return 0;
}
