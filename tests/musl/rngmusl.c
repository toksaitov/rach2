/*
 * rngmusl.c - Phase 20 integration: a musl getrandom() + /dev/urandom consumer in
 * the shape an SSH key generator (dropbear, Phase 23) uses.
 *
 * Generates two 32-byte "keys" (one via getrandom(), one via /dev/urandom)
 * and confirms both are delivered, non-zero, and distinct (independent draws). On
 * a trusted-entropy boot (RDSEED/RDRAND, the run scripts' -cpu max) both succeed;
 * on a no-hardware boot they fail closed (the rngprobe covers that path).
 *
 * Prints "RNGMUSL OK" / "RNGMUSL FAIL". Linked against the ported musl.
 */

#include <sys/random.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>

static int all_zero(const unsigned char *b, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        if (b[i]) {
            return 0;
        }
    }
    return 1;
}

int main(void)
{
    unsigned char key1[32], key2[32];
    int fd;

    if (getrandom(key1, sizeof(key1), 0) != (ssize_t)sizeof(key1)) {
        printf("RNGMUSL FAIL (getrandom)\n");
        return 1;
    }
    fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0 || read(fd, key2, sizeof(key2)) != (ssize_t)sizeof(key2)) {
        printf("RNGMUSL FAIL (urandom)\n");
        return 1;
    }
    close(fd);

    if (all_zero(key1, sizeof(key1)) || all_zero(key2, sizeof(key2))) {
        printf("RNGMUSL FAIL (zero key)\n");
        return 1;
    }
    if (memcmp(key1, key2, sizeof(key1)) == 0) {
        printf("RNGMUSL FAIL (keys identical)\n");
        return 1;
    }
    printf("RNGMUSL OK\n");
    return 0;
}
