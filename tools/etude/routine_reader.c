/* Verify the local reader against its installed preparation record. */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include "win/files.h"
#endif
#include "measurement_context.h"
#include "preparation_record.h"
#include "retention.h"
#include "routine_evidence.h"

static int reader_entry(char *line, const char *tree, const char *program,
                        const struct json *actual, size_t *matches)
{
    char *fields[6], *at = line;
    for (size_t i = 0; i < 5; i++) {
        fields[i] = at;
        char *space = strchr(at, ' ');
        if (!space)
            return ret_fail("malformed reader file record");
        *space = 0;
        at = space + 1;
    }
    fields[5] = at;
    char *given = ret_absolute(at) ? strdup(at) : ret_format("%s/%s", tree, at);
    char *path = given ? ret_path(given, 1) : NULL;
    free(given);
    if (!path)
        return ret_fail("reader record file is unavailable");
    int rc = 0;
    if (!strcmp(path, program)) {
        char *end;
        unsigned long long size = strtoull(fields[2], &end, 10);
        if (!*fields[2] || *end || fields[2][0] == '-' ||
            size != (unsigned long long)ret_number(actual, "size_bytes") ||
            strcmp(fields[1], json_text(actual, "sha256")))
            rc = ret_fail("local reader differs from its installed record");
        (*matches)++;
    }
    free(path);
    return rc;
}

static void reader_trim(char *line, ssize_t n)
{
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r'))
        line[--n] = 0;
}

static int reader_tree(FILE *file, char **tree)
{
    char *line = NULL;
    size_t capacity = 0, roots = 0;
    ssize_t n;
    int rc = 0;
    n = getline(&line, &capacity, file);
    if (n >= 0)
        reader_trim(line, n);
    if (n < 0 || strcmp(line, ETUDE_PREPARATION_HEAD))
        rc = ret_fail("reader record is not a preparation record");
    while (!rc && (n = getline(&line, &capacity, file)) >= 0) {
        reader_trim(line, n);
        if (!strncmp(line, "tree ", 5)) {
            free(*tree);
            *tree = strdup(line + 5);
            roots++;
        }
    }
    free(line);
    return rc || ferror(file) || roots != 1 ? ret_fail("reader record needs exactly one tree") : 0;
}

static int reader_verify(const char *program, const char *record)
{
    struct json *record_before = ret_descriptor(record);
    FILE *file = record_before ? fopen(record, "rb") : NULL;
    char *line = NULL, *tree = NULL;
    size_t capacity = 0, matches = 0;
    ssize_t n;
    int rc = !file || reader_tree(file, &tree);
    struct json *actual = !rc ? ret_descriptor(program) : NULL;
    if (!rc && !actual)
        rc = ret_fail("reader record needs exactly one tree and a readable program");
    if (!rc && fseek(file, 0, SEEK_SET))
        rc = -1;
    while (!rc && (n = getline(&line, &capacity, file)) >= 0) {
        reader_trim(line, n);
        if (!strncmp(line, "file ", 5))
            rc = reader_entry(line, tree, program, actual, &matches);
    }
    if (!rc && (ferror(file) || matches != 1))
        rc = ret_fail("local reader is not recorded exactly once");
    struct json *record_after = !rc ? ret_descriptor(record) : NULL;
    if (!rc && (!record_after || !json_equal(record_before, record_after)))
        rc = ret_fail("reader record changed while checking its program");
    json_free(record_before);
    json_free(record_after);
    json_free(actual);
    if (file)
        fclose(file);
    free(line);
    free(tree);
    return rc;
}

int etude_prepare_reader_main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "etude prepare reader PROGRAM RECORD\n");
        return 2;
    }
    char *program = ret_path(argv[1], 1);
    int rc = !program || reader_verify(program, argv[2]);
    if (!rc)
        puts(program);
    else
        fprintf(stderr, "etude prepare reader: %s\n", ret_error());
    free(program);
    return rc ? 2 : 0;
}
