/**
 * List a directory, one entry per line as a type flag plus the name.
 * Dotfiles are hidden.
 */

#include <stdio.h>
#include <dirent.h>

static void print_entry(const char *name, unsigned char d_type)
{
    char type;

    switch (d_type) {
    case DT_DIR:
        type = 'd';
        break;
    case DT_REG:
        type = '-';
        break;
    case DT_LNK:
        type = 'l';
        break;
    default:
        type = '?';
        break;
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
