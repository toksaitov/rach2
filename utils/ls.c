/**
 * List one directory, the first operand or the working directory, one entry
 * per line: a type letter, a fixed `rw-`, and the name.
 *
 * The type letter is `d`, `-`, `l`, or `?` for any other type. Names that
 * begin with a dot are not listed. Operands after the first are ignored.
 */

#include <stdio.h>
#include <dirent.h>
#include <errno.h>
#include <string.h>

/**
 * Print one entry's line.
 *
 * The `rw-` field is a fixed placeholder; no permissions are read.
 */
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
    printf("%crw- %s\n", type, name);
}

/**
 * List the directory. The status is 1, with a message, when it cannot be
 * opened or read, and 0 otherwise. A read error is reported with the path.
 * A failed close does not change the status.
 */
int main(int argc, char *argv[])
{
    const char *dir_path = (argc > 1) ? argv[1] : ".";
    DIR *dir;
    struct dirent *de;
    int status = 0;

    dir = opendir(dir_path);
    if (dir == NULL) {
        fputs("ls: cannot open directory\n", stderr);
        return 1;
    }

    for (;;) {
        errno = 0;
        de = readdir(dir);
        if (de == NULL) {
            break;
        }
        if (de->d_name[0] != '.') {
            print_entry(de->d_name, de->d_type);
        }
    }
    if (errno != 0) {
        fprintf(stderr, "ls: %s: %s\n", dir_path, strerror(errno));
        status = 1;
    }

    closedir(dir);
    return status;
}
