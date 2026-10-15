/**
 * ofdmusl: check that `dup2` shares one file offset between two descriptors.
 *
 * Every open creates one open file description, and `dup2` makes a second
 * descriptor refer to it, so the two share one offset. The program opens a
 * file for writing, makes it standard output with `dup2`, writes "AAA" through
 * stdio on descriptor 1, then writes "BBB" through the original descriptor.
 * With a shared offset the second write follows the first and the file reads
 * "AAABBB"; with an offset per descriptor the second write would land at
 * offset 0 and the file would read "BBB".
 *
 * It prints `OFDMUSL OK` when the file reads "AAABBB", and an
 * `OFDMUSL FAIL` line naming the step that failed otherwise. Linked
 * statically against the ported musl; see tests/musl/build.sh.
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

    savedout = dup(STDOUT_FILENO);
    if (savedout < 0 || dup2(filefd, STDOUT_FILENO) != STDOUT_FILENO) {
        fputs("OFDMUSL FAIL: dup2\n", stderr);
        return 1;
    }

    fputs("AAA", stdout);
    fflush(stdout);
    if (write(filefd, "BBB", 3) != 3) {
        dup2(savedout, STDOUT_FILENO);
        fputs("OFDMUSL FAIL: write\n", stderr);
        return 1;
    }

    dup2(savedout, STDOUT_FILENO);
    close(savedout);
    close(filefd);

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
