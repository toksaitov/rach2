/* Exercise Doom's freestanding C library through its Linux system calls. */
#include <stdint.h>
#include <limits.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>

static int failures;
static int no_compare;

/** Record a failed condition on standard error. */
static void check(int condition, const char *name)
{
    if (!condition) {
        fprintf(stderr, "doom-libc-test: fail: %s\n", name);
        failures++;
    }
}

/** Compare two integers; an empty sort must never call the comparator. */
static int compare(const void *a, const void *b)
{
    if (no_compare) {
        check(0, "empty sort calls comparator");
        _exit(1);
    }
    return (*(const int *)a > *(const int *)b) - (*(const int *)a < *(const int *)b);
}

/** Check the integer limits, exact-width format strings and conversions. */
static void integers(void)
{
    char buf[256];
    int n = 77, second = 77;
    unsigned int u = 77;
    int64_t low = INT64_MIN;
    uint64_t high = UINT64_MAX;

    check(LONG_MAX == (long)(~0UL >> 1), "long maximum");
    check(LONG_MIN == -LONG_MAX - 1L && ULONG_MAX == ~0UL, "long limits");
    check(sizeof(ssize_t) == sizeof(size_t), "ssize_t width");
    check(sizeof(off_t) == sizeof(long), "off_t width");
    snprintf(buf, sizeof(buf), "%" PRId64 " %" PRIu64 " %" PRIx64 " %" PRIX64, low, high, high,
             high);
    check(strcmp(buf, "-9223372036854775808 18446744073709551615 "
                      "ffffffffffffffff FFFFFFFFFFFFFFFF") == 0,
          "64-bit macros");
    snprintf(buf, sizeof(buf), "%lld %llu %llx %llo %d", (long long)low, (unsigned long long)high,
             (unsigned long long)high, (unsigned long long)high, 37);
    check(strcmp(buf, "-9223372036854775808 18446744073709551615 "
                      "ffffffffffffffff 1777777777777777777777 37") == 0,
          "long long output and following argument");
    printf("doom-libc-format: %" PRId64 " %" PRIu64 " %" PRIx64 " %d\n", low, high, high, 37);
    check(sscanf("2a", "%x", &u) == 1 && u == 42, "hex conversion");
    check(sscanf("0X2A", "%i", &n) == 1 && n == 42, "automatic hex base");
    check(sscanf("052", "%i", &n) == 1 && n == 42, "automatic octal base");
    check(sscanf("42", "%i", &n) == 1 && n == 42, "automatic decimal base");
    check(sscanf("52", "%o", &u) == 1 && u == 42, "octal conversion");
    check(sscanf("\t\n-42", " %d", &n) == 1 && n == -42, "signed whitespace");
    check(sscanf("42", " %d", &n) == 1 && n == 42, "zero whitespace");
    check(sscanf("\v0x2a", " 0x%x", &u) == 1 && u == 42, "engine hex format");
    check(sscanf("0X2a", " 0X%x", &u) == 1 && u == 42, "engine capital prefix");
    check(sscanf("052", " 0%o", &u) == 1 && u == 42, "engine octal format");
    check(sscanf("+42", "%u", &u) == 1 && u == 42, "unsigned sign");
    check(sscanf("42\t-7%", "%d %d%%", &n, &second) == 2 && n == 42 && second == -7,
          "multiple assignments and percent");
    n = 77;
    check(sscanf("bad", "%d", &n) == 0 && n == 77, "failed decimal");
    check(sscanf("+", "%d", &n) == 0 && n == 77, "sign without digits");
    check(sscanf(" \n", "%d", &n) == EOF && n == 77, "input exhaustion");
    check(sscanf("1 bad", "%d %d", &n, &second) == 1 && second == -7, "assignment before failure");
}

/** Check the inline helpers at their smallest valid and invalid sizes. */
static void helpers(void)
{
    char buf[2] = {'a', 'b'};
    int values[] = {3, -1, 2};

    check(getcwd(buf, 0) == NULL && buf[0] == 'a' && buf[1] == 'b', "cwd size zero");
    check(getcwd(buf, 1) == NULL && buf[0] == 'a' && buf[1] == 'b', "cwd size one");
    check(getcwd(buf, 2) == buf && strcmp(buf, "/") == 0, "cwd fits");
    check(getcwd(NULL, 2) == NULL, "cwd null");
    no_compare = 1;
    qsort(NULL, 0, sizeof(int), compare);
    qsort(values, 1, sizeof(int), compare);
    no_compare = 0;
    qsort(values, 3, sizeof(int), compare);
    check(values[0] == -1 && values[1] == 2 && values[2] == 3, "integer sort");
}

/** Check both directions of update streams on the file at `path`. */
static void streams(const char *path)
{
    char buf[4] = {0};
    FILE *f = fopen(path, "w+");

    check(f != NULL, "open w+");
    if (!f)
        return;
    check(fwrite("ab", 1, 2, f) == 2 && fseek(f, 0, SEEK_SET) == 0, "write update stream");
    check(fread(buf, 1, 2, f) == 2 && memcmp(buf, "ab", 2) == 0, "read w+");
    fclose(f);
    f = fopen(path, "a+");
    check(f != NULL, "open a+");
    if (!f)
        return;
    check(fseek(f, 0, SEEK_SET) == 0 && fwrite("c", 1, 1, f) == 1, "append after seek");
    check(fseek(f, 0, SEEK_SET) == 0 && fread(buf, 1, 3, f) == 3 && memcmp(buf, "abc", 3) == 0,
          "read a+");
    fclose(f);
    f = fopen(path, "rb+");
    check(f != NULL, "open rb+");
    if (!f)
        return;
    check(fwrite("d", 1, 1, f) == 1 && fseek(f, 0, SEEK_SET) == 0 && fread(buf, 1, 3, f) == 3 &&
              memcmp(buf, "dbc", 3) == 0,
          "binary update mode");
    fclose(f);
}

/** Run with one scratch file path; return nonzero if any check fails. */
int main(int argc, char **argv)
{
    if (argc != 2)
        return 2;
    integers();
    helpers();
    streams(argv[1]);
    fprintf(stderr, "doom-libc-test: result: %s\n", failures ? "fail" : "pass");
    return failures != 0;
}
