/**
 * Exercises a shared open-file offset through real musl dup2 and stdio.
 *
 * The kernel keeps one refcounted open-file description per open, and dup2
 * makes a second descriptor alias it. This test opens a writable file, dup2's
 * it over stdout, writes "AAA" with printf through fd 1, then writes "BBB"
 * through the original descriptor. The two descriptors share one seek offset,
 * so the second write appends instead of overwriting: read back, the file is
 * "AAABBB" (6 bytes). If each descriptor had its own offset (the old bug), the
 * second write would land at offset 0 and the file would read just "BBB".
 *
 * Prints "OFDMUSL OK" when dup2 carried the offset end to end through libc, or
 * an "OFDMUSL FAIL: ..." line naming the step that broke. Linked statically
 * against the ported musl; see tests/musl/build.sh.
 */

#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

#define PATH "/ofdmusl.txt"

int main(void)
{
    int filefd, savedout, rfd;
    char buf[16];
    ssize_t n;

    filefd = open(PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (filefd < 0) {
        fputs("OFDMUSL FAIL: open\n", stderr);
        return 1;
    }

    /* Redirect stdout to the file, saving a copy to restore afterwards. */
    savedout = dup(STDOUT_FILENO);
    if (savedout < 0 || dup2(filefd, STDOUT_FILENO) != STDOUT_FILENO) {
        fputs("OFDMUSL FAIL: dup2\n", stderr);
        return 1;
    }

    /* Write through stdout (fd 1, aliasing filefd), then through filefd. */
    fputs("AAA", stdout);
    fflush(stdout);
    if (write(filefd, "BBB", 3) != 3) {
        dup2(savedout, STDOUT_FILENO);
        fputs("OFDMUSL FAIL: write\n", stderr);
        return 1;
    }

    /* Restore the console stdout and tear down the writers. */
    dup2(savedout, STDOUT_FILENO);
    close(savedout);
    close(filefd);

    /* Read it back: a shared offset gives "AAABBB"; an unshared one "BBB". */
    rfd = open(PATH, O_RDONLY);
    if (rfd < 0) {
        fputs("OFDMUSL FAIL: reopen\n", stderr);
        return 1;
    }
    n = read(rfd, buf, sizeof buf - 1);
    close(rfd);
    if (n < 0) {
        n = 0;
    }
    buf[n] = '\0';

    if (n == 6 && strcmp(buf, "AAABBB") == 0) {
        printf("OFDMUSL OK (file=[%s])\n", buf);
        return 0;
    }
    printf("OFDMUSL FAIL: file=[%s] (offset not shared)\n", buf);
    return 1;
}
