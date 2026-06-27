/**
 * Drives the ported musl file layer over the hierarchical KRFFS, end to end
 * in-guest, the way a real file utility would.
 *
 * The program reads a pre-staged root file (`/hello.txt`) through `<stdio.h>`,
 * then a host-staged nested file (`/etc/motd`, written into the image by the
 * manifest) and lists `/etc` for `motd` and `hostname`, proving musl navigates
 * a tree staged before boot rather than only one built at runtime.
 * It then makes a fresh directory and creates and writes a file inside it,
 * reads that nested file back and verifies its bytes, stats it as a regular
 * file of exactly the bytes written, opendirs the directory and confirms
 * readdir lists the file, and finally unlinks the file and rmdirs the
 * now-empty directory. musl routes these through open, read, write, mkdir,
 * rmdir, and unlink and the 64-bit stat64 and getdents64 syscalls.
 *
 * Prints "CATFILE OK" only if every step matched, otherwise a "CATFILE FAIL"
 * line naming the step. Linked statically against the ported musl; see
 * tests/musl/build.sh.
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

    /* Pre-staged root file through stdio. */
    f = fopen("/hello.txt", "r");
    if (!f) { printf("CATFILE FAIL: open /hello.txt\n"); return 1; }
    if (!fgets(buf, sizeof buf, f) || strncmp(buf, "hello, world", 12) != 0) {
        printf("CATFILE FAIL: read /hello.txt got \"%s\"\n", buf);
        fclose(f);
        return 1;
    }
    fclose(f);

    /* Host-staged nested file, then list /etc. */
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

    /* Make a directory and create+write a file inside it. */
    if (mkdir("/cf", 0755) != 0) { printf("CATFILE FAIL: mkdir /cf\n"); return 1; }
    f = fopen("/cf/data", "w");
    if (!f) { printf("CATFILE FAIL: create /cf/data\n"); return 1; }
    if (fputs(PAYLOAD, f) < 0) {
        printf("CATFILE FAIL: write /cf/data\n");
        fclose(f);
        return 1;
    }
    fclose(f);

    /* Read the nested file back and verify its contents. */
    f = fopen("/cf/data", "r");
    if (!f) { printf("CATFILE FAIL: reopen /cf/data\n"); return 1; }
    if (!fgets(buf, sizeof buf, f) || strcmp(buf, PAYLOAD) != 0) {
        printf("CATFILE FAIL: read /cf/data got \"%s\"\n", buf);
        fclose(f);
        return 1;
    }
    fclose(f);

    /* stat the nested file: a regular file of exactly the bytes we wrote. */
    if (stat("/cf/data", &st) != 0 || !S_ISREG(st.st_mode) ||
        st.st_size != (off_t)strlen(PAYLOAD)) {
        printf("CATFILE FAIL: stat /cf/data\n");
        return 1;
    }

    /* opendir/readdir the directory and confirm it lists the file. */
    d = opendir("/cf");
    if (!d) { printf("CATFILE FAIL: opendir /cf\n"); return 1; }
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, "data") == 0) {
            saw_data = 1;
        }
    }
    closedir(d);
    if (!saw_data) { printf("CATFILE FAIL: readdir /cf missing data\n"); return 1; }

    /* Clean up: the file, then the now-empty directory. */
    if (unlink("/cf/data") != 0) { printf("CATFILE FAIL: unlink /cf/data\n"); return 1; }
    if (rmdir("/cf") != 0) { printf("CATFILE FAIL: rmdir /cf\n"); return 1; }

    printf("CATFILE OK\n");
    return 0;
}
