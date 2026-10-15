/**
 * rngmusl: draw random bytes through the ported musl, as an SSH key generator
 * does, and check what the kernel's random source gives.
 *
 * Fills two 32-byte buffers, one through musl's `getrandom()` and one by
 * reading `/dev/urandom`, then confirms both calls deliver the full request,
 * neither buffer is all zero, and the two draws differ (so the source is not a
 * constant). On a trusted-entropy boot (RDSEED or RDRAND, the run scripts'
 * `-cpu max`) both paths succeed; on a no-hardware boot the kernel fails them
 * closed and the rngprobe covers that path.
 *
 * Prints "RNGMUSL OK" on success or an "RNGMUSL FAIL" line naming the failing
 * step. No suite runs it; it is run by hand. Linked against the ported musl.
 */

#include <sys/random.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>

/**
 * Return 1 when all `n` bytes at `b` are zero, and 0 otherwise.
 */
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
