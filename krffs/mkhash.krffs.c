/*
 * mkhash.krffs.c: mkhash.krffs, which makes the SHA-512 crypt password hashes
 * the images stage in /etc/shadow.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * The SHA-512 crypt of the vendored musl,
 * vendor/musl/musl-1.2.5/src/crypt/crypt_sha512.c, which the Makefile compiles
 * beside this tool; `__crypt_sha512` is the name musl's own `crypt` calls it
 * by. A hash made on the host is so checked on the guest by the same code.
 */
char *__crypt_sha512(const char *key, const char *setting, char *output);

/**
 * mkhash.krffs SETTING: prints the SHA-512 crypt hash of a password for
 * `/etc/shadow`, where SETTING is `$6$` and a salt.
 *
 * The password is the first line of stdin, up to the first CR or LF, so it
 * does not appear in the process arguments. The hash goes to stdout as one
 * line. Returns `EXIT_SUCCESS`, or `EXIT_FAILURE` for a usage error, a
 * password that is empty or longer than 255 bytes, or a setting `crypt`
 * rejects.
 */
int main(int argc, char **argv)
{
    char password[256];
    char output[256];
    char *hash;
    size_t length;

    if (argc != 2 || strncmp(argv[1], "$6$", 3) != 0) {
        fprintf(stderr, "mkhash: usage: mkhash.krffs '$6$salt' < password\n");
        return EXIT_FAILURE;
    }

    if (fgets(password, sizeof(password), stdin) == NULL) {
        fprintf(stderr, "mkhash: no password on stdin\n");
        return EXIT_FAILURE;
    }
    if (strchr(password, '\n') == NULL) {
        int next = fgetc(stdin);

        if (next != EOF && next != '\n') {
            fprintf(stderr, "mkhash: password longer than %d bytes\n", (int)sizeof(password) - 1);
            return EXIT_FAILURE;
        }
    }

    length = strcspn(password, "\r\n");
    password[length] = '\0';
    if (length == 0) {
        fprintf(stderr, "mkhash: empty password\n");
        return EXIT_FAILURE;
    }

    hash = __crypt_sha512(password, argv[1], output);
    if (hash == NULL || hash[0] == '*') {
        fprintf(stderr, "mkhash: crypt rejected the setting\n");
        return EXIT_FAILURE;
    }

    printf("%s\n", hash);
    return EXIT_SUCCESS;
}
