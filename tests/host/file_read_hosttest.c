/** Test cp, cat and ls read failures with in-memory input and output. */

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int input_error;
static int input_left;
static int closes;
static char output[128];
static char diagnostic[256];
static struct dirent entry;

/** Open the input or output descriptor without using host files. */
static int model_open(const char *path, int flags, ...)
{
    (void)path;
    return flags & O_WRONLY ? 11 : 10;
}

/** Return the selected number of data blocks, then EOF or EIO. */
static ssize_t model_read(int fd, void *bytes, size_t size)
{
    assert(fd == 0 || fd == 10);
    assert(size >= 4);
    if (input_left > 0) {
        --input_left;
        memcpy(bytes, "data", 4);
        return 4;
    }
    if (input_error) {
        errno = EIO;
        return -1;
    }
    return 0;
}

/** Record writes, completing at most two bytes per data write. */
static ssize_t model_write(int fd, const void *bytes, size_t size)
{
    char *dest = fd == STDERR_FILENO ? diagnostic : output;
    size_t used = strlen(dest);
    size_t capacity = fd == STDERR_FILENO ? sizeof(diagnostic) : sizeof(output);

    if (fd != STDERR_FILENO && size > 2) {
        size = 2;
    }
    assert(used + size < capacity);
    memcpy(dest + used, bytes, size);
    dest[used + size] = '\0';
    return (ssize_t)size;
}

/** Count releases and change errno to check that errors survive cleanup. */
static int model_close(int fd)
{
    assert(fd == 10 || fd == 11);
    ++closes;
    errno = EBADF;
    return 0;
}

/** Return an opaque directory handle. */
static DIR *model_opendir(const char *path)
{
    (void)path;
    return (DIR *)&entry;
}

/** Return entries, then EOF without changing errno, or EIO. */
static struct dirent *model_readdir(DIR *dir)
{
    assert(dir == (DIR *)&entry);
    if (input_left > 0) {
        --input_left;
        strcpy(entry.d_name, "file");
        entry.d_type = DT_REG;
        return &entry;
    }
    if (input_error) {
        errno = EIO;
    }
    return NULL;
}

/** Count the directory release and change errno as close does. */
static int model_closedir(DIR *dir)
{
    assert(dir == (DIR *)&entry);
    ++closes;
    errno = EBADF;
    return 0;
}

/** Record a diagnostic in place of fprintf to stderr. */
static int model_fprintf(FILE *stream, const char *format, ...)
{
    va_list ap;
    int result;

    assert(stream == stderr);
    va_start(ap, format);
    result = vsnprintf(diagnostic, sizeof(diagnostic), format, ap);
    va_end(ap);
    return result;
}

/** Record directory output; a successful print may change errno. */
static int model_printf(const char *format, ...)
{
    va_list ap;
    int result;

    va_start(ap, format);
    result = vsnprintf(output, sizeof(output), format, ap);
    va_end(ap);
    errno = ENOSPC;
    return result;
}

#define open    model_open
#define read    model_read
#define write   model_write
#define close   model_close
#define fprintf model_fprintf
#define main    cp_main
#define buffer  cp_buffer
#include "cp.c"
#undef buffer
#undef main
#define main   cat_main
#define buffer cat_buffer
#include "cat.c"
#undef buffer
#undef main
#define main     ls_main
#define opendir  model_opendir
#define readdir  model_readdir
#define closedir model_closedir
#define printf   model_printf
#include "ls.c"
#undef printf
#undef closedir
#undef readdir
#undef opendir
#undef main
#undef fprintf
#undef close
#undef write
#undef read
#undef open

/** Check status, diagnostic, partial output and descriptor releases. */
static int run_case(const char *program, int blocks, int error, int implicit)
{
    char *cp_args[] = {"cp", "source", "destination", NULL};
    char *cat_args[] = {"cat", "source", NULL};
    char *ls_args[] = {"ls", "directory", NULL};
    char expected[256];
    const char *path = "source";
    const char *bytes = blocks ? "data" : "";
    int expected_closes = 1;
    int status;

    input_left = blocks;
    input_error = error;
    closes = 0;
    output[0] = diagnostic[0] = '\0';
    errno = EACCES;
    if (strcmp(program, "cp") == 0) {
        status = cp_main(3, cp_args);
        expected_closes = 2;
    } else if (strcmp(program, "cat") == 0) {
        status = cat_main(implicit ? 1 : 2, cat_args);
        if (implicit) {
            path = "standard input";
            expected_closes = 0;
        }
    } else {
        status = ls_main(implicit ? 1 : 2, ls_args);
        path = implicit ? "." : "directory";
        bytes = blocks ? "-rw- file\n" : "";
    }
    expected[0] = '\0';
    if (error) {
        snprintf(expected, sizeof(expected), "%s: %s: %s\n", program, path, strerror(EIO));
    }
    if (status != error || strcmp(diagnostic, expected) != 0 || strcmp(output, bytes) != 0 ||
        closes != expected_closes) {
        fprintf(stderr, "file-read-hosttest: fail: %s blocks %d error %d implicit %d\n", program,
                blocks, error, implicit);
        return 1;
    }
    return 0;
}

/** Check EOF and EIO before and after input, including implicit inputs. */
int main(void)
{
    int blocks, error, implicit;
    int failures = 0;

    for (blocks = 0; blocks <= 1; ++blocks) {
        for (error = 0; error <= 1; ++error) {
            failures += run_case("cp", blocks, error, 0);
            for (implicit = 0; implicit <= 1; ++implicit) {
                failures += run_case("cat", blocks, error, implicit);
                failures += run_case("ls", blocks, error, implicit);
            }
        }
    }
    if (failures) {
        fprintf(stderr, "file-read-hosttest: result: fail: %d cases\n", failures);
        return 1;
    }
    puts("file-read-hosttest: result: pass: 20 cases");
    return 0;
}
