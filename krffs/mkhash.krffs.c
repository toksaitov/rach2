#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The vendored musl SHA-512 crypt (vendor/musl/musl-1.2.5/src/crypt/
   crypt_sha512.c), compiled beside this tool by the Makefile under the
   internal name musl exports it to its own crypt(). The host mints hashes
   with exactly the code the guest verifies them with. */
char *__crypt_sha512(const char *key, const char *setting, char *output);

/**
 * Mints a SHA-512 crypt (`$6$`) password hash for `/etc/shadow`.
 *
 * Usage: `mkhash.krffs <setting>`, where `<setting>` is a `$6$salt` prefix.
 * The password is the first line of stdin, so it never appears in the
 * process arguments. The hash goes to stdout as one line.
 *
 * @return `EXIT_SUCCESS` with the hash on stdout, `EXIT_FAILURE` for a usage
 *         error, an empty password, or a setting `crypt` rejects.
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
