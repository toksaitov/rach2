/*
 * ofdmusl.c - Phase 11 integration milestone: shared open-file offset via musl.
 *
 * Proves musl's dup2 + buffered stdio ride the kernel's refcounted open-file
 * description: it dup2's a writable file over stdout, writes "AAA" with printf
 * (through fd 1), then writes "BBB" through the ORIGINAL file descriptor. Because
 * the two descriptors share one seek offset, the second write APPENDS: the file
 * reads back "AAABBB" (6 bytes). If each fd had its own offset (the old bug), the
 * second write would overwrite at offset 0 and the file would be just "BBB".
 * Printing "OFDMUSL OK" means dup2 shared the offset end to end through libc.
 *
 * Linked statically against the ported musl; see tests/musl/build.sh.
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

    /* Redirect stdout to the file, keeping a saved copy to restore afterwards. */
    savedout = dup(STDOUT_FILENO);
    if (savedout < 0 || dup2(filefd, STDOUT_FILENO) != STDOUT_FILENO) {
        fputs("OFDMUSL FAIL: dup2\n", stderr);
        return 1;
    }

    /* Write through stdout (fd 1, sharing filefd's offset), then through filefd. */
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

    /* Read the file back: a shared offset gives "AAABBB"; an unshared one "BBB". */
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
