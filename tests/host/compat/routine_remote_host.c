/* Use this fixture as an SSH stand-in and a staged native child. */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif
#endif
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include "fixdir_host.h"
#include "../../../tools/etude/retention.h"
#include "../../../tools/etude/routine_remote.h"
#include "../../../tools/etude/include/etude/remote.h"

static int routine_failures;
#define ROUTINE_CHECK(test)                                                                    \
    do {                                                                                       \
        if (!(test)) {                                                                         \
            fprintf(stderr, "routine remote fixture: %s:%d: %s\n", __FILE__, __LINE__, #test); \
            routine_failures++;                                                                \
        }                                                                                      \
    } while (0)

int etude_test_routine_remote_child(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "--routine-child")) {
        FILE *file = fopen("proof.txt", "wb");
        if (!file || fputs("actual native proof bytes\n", file) < 0 || fclose(file))
            return 2;
        puts("actual native output");
        fputs("actual native stderr\n", stderr);
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "--routine-fail"))
        return 7;
    const char *base = strrchr(argv[0], '/');
    if (strcmp(base ? base + 1 : argv[0], "ssh"))
        return -1;
    if (argc != 5 || strcmp(argv[1], "-oBatchMode=yes") || strcmp(argv[2], "--") ||
        strcmp(argv[3], "fixture") || !strstr(argv[4], " serve --stdio -d "))
        return 90;
    const char *fail = getenv("ROUTINE_REMOTE_FAILURE");
    if (fail && !strcmp(fail, "handshake"))
        return 1;
    struct etude_serve_conf conf = {0};
    conf.in = 0;
    conf.out = 1;
    conf.state = getenv("ROUTINE_REMOTE_STATE");
    conf.boot = "0123456789abcdef";
    conf.log = stderr;
    int status = etude_serve(&conf);
    return fail && !strcmp(fail, "exit") ? 7 : status;
}

static void routine_test_close(struct json *producer, const char *root, const char *mode)
{
    char *wire = ret_format("%s/%s.wire", root, mode);
    if (strcmp(mode, "abandon"))
        setenv("ROUTINE_REMOTE_FAILURE", mode, 1);
    else
        unsetenv("ROUTINE_REMOTE_FAILURE");
    struct etude_routine_remote *remote =
        etude_routine_remote_open(producer, mode, wire, NULL, NULL, NULL);
    if (!strcmp(mode, "handshake"))
        ROUTINE_CHECK(remote == NULL);
    else {
        ROUTINE_CHECK(remote != NULL);
        if (remote) {
            int result = !strcmp(mode, "abandon") ? etude_routine_remote_abandon(&remote)
                                                  : etude_routine_remote_close(&remote);
            ROUTINE_CHECK(!strcmp(mode, "abandon") ? result == 0 : result != 0);
            ROUTINE_CHECK(remote == NULL);
        }
    }
    free(wire);
    unsetenv("ROUTINE_REMOTE_FAILURE");
}

static char *routine_test_bytes(const char *path, size_t *size)
{
    struct stat st;
    FILE *file = fopen(path, "rb");
    if (!file || fstat(fileno(file), &st) || st.st_size < 0)
        exit(2);
    *size = (size_t)st.st_size;
    char *bytes = json_alloc(*size + 1);
    if (fread(bytes, 1, *size, file) != *size || fclose(file))
        exit(2);
    return bytes;
}

static void routine_test_execute(struct json *producer, const char *root, const char *self)
{
    char *wire = ret_format("%s/native.wire", root), *output = ret_format("%s/native.log", root);
    struct etude_routine_remote *remote =
        etude_routine_remote_open(producer, "native", wire, NULL, NULL, NULL);
    ROUTINE_CHECK(remote != NULL);
    struct json *arguments = ret_new(JSON_ARRAY), *environment = ret_new(JSON_OBJECT);
    ret_push(arguments, ret_string("--routine-child"));
    const char *keys[] = {"ASAN_OPTIONS", "ASAN_SYMBOLIZER_PATH", "UBSAN_OPTIONS"};
    for (size_t i = 0; i < 3; i++)
        if (getenv(keys[i]))
            ret_set(environment, keys[i], ret_string(getenv(keys[i])));
    size_t size;
    char *program = routine_test_bytes(self, &size);
    if (remote) {
        ROUTINE_CHECK(!etude_routine_remote_put(remote, "native-child", program, size, "0755"));
        const char *token = etude_routine_remote_execute(remote, "native", "native-child",
                                                         arguments, environment, output);
        ROUTINE_CHECK(token != NULL);
        if (token) {
            char *bytes = NULL;
            size_t length = 0;
            ROUTINE_CHECK(
                !etude_routine_remote_get(remote, token, "proof.txt", 65536, &bytes, &length));
            ROUTINE_CHECK(length == 26 && !memcmp(bytes, "actual native proof bytes\n", length));
            free(bytes);
        }
        struct json *changed = ret_new(JSON_ARRAY);
        ret_push(changed, ret_string("--routine-fail"));
        char *failed = ret_format("%s/failed.log", root);
        ROUTINE_CHECK(etude_routine_remote_execute(remote, "failed", "native-child", changed,
                                                   environment, failed) == NULL);
        char *result_path = ret_format("%s.result.json", failed);
        struct json *result = ret_document(result_path);
        ROUTINE_CHECK(result && !strcmp(json_text(result, "value"), "7") &&
                      !strcmp(json_text(result, "cleanup"), "confirmed"));
        json_free(result);
        json_free(changed);
        free(failed);
        free(result_path);
        ROUTINE_CHECK(!etude_routine_remote_close(&remote));
        ROUTINE_CHECK(remote == NULL);
    }
    free(program);
    json_free(arguments);
    json_free(environment);
    free(wire);
    free(output);
}

/**
 * Check routine staging and cleanup with this executable as the SSH stand-in.
 * A symbolic link works when the executable and scratch use different
 * filesystems.
 */
int etude_test_routine_remote(const char *self)
{
    char *root = ret_path(fix_dir(), 1), *bin = ret_format("%s/bin", root);
    ROUTINE_CHECK(!ret_dirs(bin));
    char *ssh = ret_format("%s/ssh", bin);
    ROUTINE_CHECK(!symlink(self, ssh));
    char *old_path = getenv("PATH") ? strdup(getenv("PATH")) : NULL;
    char *path = ret_format("%s:%s", bin, old_path ? old_path : "");
    setenv("PATH", path, 1);
    char *state = ret_format("%s/state", root);
    setenv("ROUTINE_REMOTE_STATE", state, 1);
    struct json *producer = ret_new(JSON_OBJECT);
    ret_set(producer, "transport", ret_string("ssh"));
    ret_set(producer, "host", ret_string("fixture"));
    ret_set(producer, "etude", ret_string("/fixture/etude"));
    ret_set(producer, "state_root", ret_string(state));
    routine_test_execute(producer, root, self);
    routine_test_close(producer, root, "handshake");
    routine_test_close(producer, root, "exit");
    routine_test_close(producer, root, "abandon");
    ret_set(producer, "transport", ret_string("wsl"));
    char *refused = ret_format("%s/refused.wire", root);
    ROUTINE_CHECK(etude_routine_remote_open(producer, "refused", refused, "windows-import",
                                            "C:/Windows/System32/wsl.exe", NULL) == NULL);
    ROUTINE_CHECK(access(refused, F_OK) != 0);
    if (old_path)
        setenv("PATH", old_path, 1);
    else
        unsetenv("PATH");
    unsetenv("ROUTINE_REMOTE_STATE");
    json_free(producer);
    free(root);
    free(bin);
    free(ssh);
    free(old_path);
    free(path);
    free(state);
    free(refused);
    int status;
    errno = 0;
    ROUTINE_CHECK(waitpid(-1, &status, WNOHANG) == -1 && errno == ECHILD);
    if (!routine_failures)
        puts("routine remote: native staging, streams, proof and cleanup checks passed");
    return routine_failures != 0;
}
