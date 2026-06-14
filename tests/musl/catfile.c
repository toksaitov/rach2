/*
 * catfile.c - Phase 6 integration milestone: directories and files under musl.
 *
 * An ordinary hosted C program linked against the ported musl. It proves the
 * libc file layer drives the hierarchical KRFFS end to end in-guest, exactly as
 * a real file utility would:
 *   1. read a pre-staged root file (/hello.txt) through <stdio.h>,
 *   2. mkdir a fresh directory and create+write a file INSIDE it,
 *   3. read that nested file back and verify its bytes,
 *   4. stat the nested file (a regular file of the right size),
 *   5. opendir the directory and confirm readdir lists the file,
 *   6. unlink the file and rmdir the now-empty directory.
 * musl routes these through open/read/write/mkdir/rmdir/unlink and the 64-bit
 * stat64/getdents64 syscalls. Prints "CATFILE OK" only if every step matched.
 *
 * Linked statically against the ported musl; see tests/musl/build.sh.
 */

#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>

static const char PAYLOAD[] = "nested payload\n";

int main(void)
{
    char buf[64];
    FILE *f;
    struct stat st;
    DIR *d;
    struct dirent *de;
    int saw_data = 0;

    /* 1. Read a pre-staged root file through stdio. */
    f = fopen("/hello.txt", "r");
    if (!f) { printf("CATFILE FAIL: open /hello.txt\n"); return 1; }
    if (!fgets(buf, sizeof buf, f) || strncmp(buf, "hello, world", 12) != 0) {
        printf("CATFILE FAIL: read /hello.txt got \"%s\"\n", buf);
        fclose(f);
        return 1;
    }
    fclose(f);

    /* 1b. Read a HOST-staged nested file (/etc/motd, written into the image by
     * stagetree.krffs) and list /etc -- proving musl navigates a pre-staged tree,
     * not only one created at runtime. */
    buf[0] = '\0'; /* so a read failure does not print stale /hello.txt bytes */
    f = fopen("/etc/motd", "r");
    if (!f) { printf("CATFILE FAIL: open /etc/motd\n"); return 1; }
    if (!fgets(buf, sizeof buf, f) || strncmp(buf, "Welcome to Rach2", 16) != 0) {
        printf("CATFILE FAIL: read /etc/motd got \"%s\"\n", buf);
        fclose(f);
        return 1;
    }
    fclose(f);
    {
        int saw_motd = 0, saw_hostname = 0;
        d = opendir("/etc");
        if (!d) { printf("CATFILE FAIL: opendir /etc\n"); return 1; }
        errno = 0;
        while ((de = readdir(d)) != NULL) {
            if (strcmp(de->d_name, "motd") == 0) { saw_motd = 1; }
            if (strcmp(de->d_name, "hostname") == 0) { saw_hostname = 1; }
        }
        if (errno != 0) { printf("CATFILE FAIL: readdir /etc error\n"); closedir(d); return 1; }
        if (closedir(d) != 0) { printf("CATFILE FAIL: closedir /etc\n"); return 1; }
        if (!saw_motd || !saw_hostname) {
            printf("CATFILE FAIL: readdir /etc missing motd/hostname\n");
            return 1;
        }
    }

    /* 2. Make a directory and create+write a file inside it. */
    if (mkdir("/cf", 0755) != 0) { printf("CATFILE FAIL: mkdir /cf\n"); return 1; }
    f = fopen("/cf/data", "w");
    if (!f) { printf("CATFILE FAIL: create /cf/data\n"); return 1; }
    if (fputs(PAYLOAD, f) < 0) {
        printf("CATFILE FAIL: write /cf/data\n");
        fclose(f);
        return 1;
    }
    fclose(f);

    /* 3. Read the nested file back and verify its contents. */
    f = fopen("/cf/data", "r");
    if (!f) { printf("CATFILE FAIL: reopen /cf/data\n"); return 1; }
    if (!fgets(buf, sizeof buf, f) || strcmp(buf, PAYLOAD) != 0) {
        printf("CATFILE FAIL: read /cf/data got \"%s\"\n", buf);
        fclose(f);
        return 1;
    }
    fclose(f);

    /* 4. stat the nested file: a regular file of exactly the bytes we wrote. */
    if (stat("/cf/data", &st) != 0 || !S_ISREG(st.st_mode) ||
        st.st_size != (off_t)strlen(PAYLOAD)) {
        printf("CATFILE FAIL: stat /cf/data\n");
        return 1;
    }

    /* 5. opendir/readdir the directory and confirm it lists the file. */
    d = opendir("/cf");
    if (!d) { printf("CATFILE FAIL: opendir /cf\n"); return 1; }
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, "data") == 0) {
            saw_data = 1;
        }
    }
    closedir(d);
    if (!saw_data) { printf("CATFILE FAIL: readdir /cf missing data\n"); return 1; }

    /* 6. Clean up: the file, then the now-empty directory. */
    if (unlink("/cf/data") != 0) { printf("CATFILE FAIL: unlink /cf/data\n"); return 1; }
    if (rmdir("/cf") != 0) { printf("CATFILE FAIL: rmdir /cf\n"); return 1; }

    printf("CATFILE OK\n");
    return 0;
}
