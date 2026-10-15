/**
 * hdaprobe: check the `/proc/audio` report and run the staged tone diagnostic.
 *
 * The report must be one file in `/proc`, with the inode its directory entry
 * gives, mode 0444, and a descriptor link that reads back its path. Run with
 * no argument on a machine with an HDA controller, it plays a one-second
 * 440 Hz tone at 32 kHz through `/bin/audiotest`. It then checks that a
 * report opened before the tone still shows no stream, since procfs makes the
 * text when the file is opened, and that a fresh report shows the stream
 * stopped, with 32000 Hz requested and programmed and the converter's format
 * (16-bit stereo at 32 kHz) read back. Run as `hdaprobe absent` under
 * `rach2.noaudio`, it checks the file the same way and then that the report
 * shows no controller ready and nothing playing.
 *
 * Prints "HDAPROBE OK: ..." on success or "HDAPROBE FAIL: <row>" for the first
 * failed check. No suite runs it; `tests/system/hda_mutate.sh` boots it and
 * requires a failure at "open /proc/audio" once the report is renamed.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/* The last report read, terminated. */
static char text[8192];

/**
 * Print "HDAPROBE FAIL: `row`" and exit with status 1 unless `ok` is true.
 *
 * The row is the name `hda_mutate.sh` looks for in the serial log.
 */
static void require(int ok, const char *row)
{
    if (!ok) {
        printf("HDAPROBE FAIL: %s\n", row);
        exit(1);
    }
}

/**
 * Read the report open on `fd` to its end into `text`, and terminate it.
 *
 * The reads are seven bytes each, so the report is served from many offsets
 * and its end of file is reached by a read of its own. Fails the probe when
 * the report does not fit in `text` or a read fails.
 */
static void read_report(int fd)
{
    int n;
    size_t used = 0;

    while ((n = read(fd, text + used, 7)) > 0) {
        used += (size_t)n;
        require(used + 8 < sizeof(text), "report fits");
    }
    require(n == 0, "report eof");
    text[used] = 0;
}

/**
 * Run the checks; an argument "absent" selects those of a boot with no audio
 * controller. Returns 0 when every check passes and exits 1 at the first that
 * fails.
 */
int main(int argc, char **argv)
{
    struct stat path, opened;
    struct dirent *entry;
    DIR *dir;
    int fd, old, found = 0, pid, status;
    unsigned requested, programmed, format, converter;
    const char *stream;
    char linkpath[64], target[64];
    int length;
    int absent = argc > 1 && strcmp(argv[1], "absent") == 0;

    fd = open("/proc/audio", O_RDONLY);
    require(fd >= 0, "open /proc/audio");
    require(stat("/proc/audio", &path) == 0 && fstat(fd, &opened) == 0, "stat report");
    snprintf(linkpath, sizeof(linkpath), "/proc/self/fd/%d", fd);
    length = readlink(linkpath, target, sizeof(target) - 1);
    require(length >= 0, "readlink report");
    target[length] = 0;
    require(strcmp(target, "/proc/audio") == 0, "report fd link");
    require(path.st_ino == opened.st_ino && (path.st_mode & 0777) == 0444, "stat identity");
    dir = opendir("/proc");
    require(dir != NULL, "open proc directory");
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, "audio") == 0) {
            require(entry->d_ino == path.st_ino, "directory inode");
            found++;
        }
    }
    closedir(dir);
    require(found == 1, "directory lists audio once");
    read_report(fd);
    close(fd);
    if (absent) {
        require(strstr(text, "hda ready 0 playing 0") != NULL, "noaudio report");
        puts("HDAPROBE OK: noaudio report");
        return 0;
    }
    require(strstr(text, "hda ready 1 playing 0") != NULL, "enumerated report");
    require(strstr(text, "caps dac ") != NULL && strstr(text, "expanded ") != NULL,
            "codec capabilities and path");
    old = open("/proc/audio", O_RDONLY);
    require(old >= 0, "old snapshot");
    pid = fork();
    require(pid >= 0, "fork diagnostic");
    if (pid == 0) {
        int nullfd = open("/dev/null", O_RDONLY);
        require(nullfd >= 0 && dup2(nullfd, 0) == 0, "redirect diagnostic stdin");
        close(nullfd);
        execl("/bin/audiotest", "audiotest", "32000", "1", "440", (char *)NULL);
        _exit(127);
    }
    require(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0,
            "staged diagnostic");
    read_report(old);
    close(old);
    require(strstr(text, "stream requested 0 programmed 0") != NULL, "old snapshot unchanged");
    fd = open("/proc/audio", O_RDONLY);
    require(fd >= 0, "reopen report");
    read_report(fd);
    close(fd);
    require(strstr(text, "hda ready 1 playing 0") != NULL, "diagnostic stopped stream");
    stream = strstr(text, "stream requested ");
    require(stream != NULL &&
                sscanf(stream, "stream requested %u programmed %u format %x dac-format %x",
                       &requested, &programmed, &format, &converter) == 4,
            "stream report fields");
    require(requested == 32000 && programmed == 32000 && format == 0x0A11 && converter == format,
            "programmed rate and readback");
    puts(text);
    puts("HDAPROBE OK: staged tone and proc snapshot");
    return 0;
}
