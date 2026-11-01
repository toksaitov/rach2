/*
 * airplane_hosttest.c: the airplane key's action, krffs/acpi/wifi.script,
 * run by the host's /bin/sh over radio switches of its own.
 *
 * host-deps: tests/host/compat/fixdir_host.c
 * host-inputs: krffs/acpi/wifi.script
 *
 * The script reads the switches from the directory RFKILL_CLASS names, and
 * takes no lock with ACPI_LOCK empty, since a host may lack flock(1). Each
 * row makes that directory in the test's fixture directory
 * (compat/fixdir_host.c) with switches that have the files of
 * /sys/class/rfkill, and puts first on PATH a stand-in for BusyBox's rfkill,
 * which logs its arguments and sets the soft block of every Wi-Fi switch of
 * the class as `rfkill block wifi` and `unblock wifi` do. The rows check two
 * presses, which block and then unblock; switches that disagree, which are
 * all blocked; a hard block, which does not count as off; and a class with
 * no Wi-Fi switch, or none at all, on which rfkill is not run.
 */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "fixdir_host.h"

#define SCRIPT "krffs/acpi/wifi.script"

/* The stand-in for rfkill: its arguments to the log, then the soft block of
 * every Wi-Fi switch of the class. */
#define FAKE_RFKILL                                                      \
    "#!/bin/sh\n"                                                        \
    "echo \"$*\" >> \"$RFKILL_LOG\"\n"                                   \
    "case $1 in block) v=1 ;; unblock) v=0 ;; *) exit 1 ;; esac\n"       \
    "[ \"$2\" = wifi ] || exit 1\n"                                      \
    "for sw in \"$RFKILL_CLASS\"/rfkill*; do\n"                          \
    "    [ \"$(cat \"$sw/type\")\" = wlan ] && echo $v > \"$sw/soft\"\n" \
    "done\n"                                                             \
    "exit 0\n"

/** The number of failed checks, which is the exit status. */
static int fails;

/** The switch class, the stand-in's directory and its log. */
static const char *class_dir, *bin_dir, *log_path;

/** Count and print a failure when `cond` is false; `what` says what held. */
#define CHECK(cond, what)                   \
    do {                                    \
        if (!(cond)) {                      \
            fails++;                        \
            printf("  FAIL: %s\n", (what)); \
        }                                   \
    } while (0)

/** Remove the directory `path` and what is in it, with rm(1). */
static void remove_tree(const char *path)
{
    pid_t pid = fork();
    int status;

    if (pid == 0) {
        execl("/bin/rm", "rm", "-rf", path, (char *)NULL);
        _exit(127);
    }
    if (pid > 0) {
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }
    }
}

/** Make the directory `path`. Exits the test on a failure. */
static void make_dir(const char *path)
{
    if (mkdir(path, 0755) != 0 && errno != EEXIST) {
        printf("  FAIL: cannot make %s\n", path);
        exit(1);
    }
}

/** Write `text` to the file `dir`/`name`. Exits the test on a failure. */
static void put(const char *dir, const char *name, const char *text)
{
    char path[4400];
    FILE *f;

    snprintf(path, sizeof(path), "%s/%s", dir, name);
    f = fopen(path, "w");
    if (!f || fputs(text, f) < 0 || fclose(f) != 0) {
        printf("  FAIL: cannot write %s\n", path);
        exit(1);
    }
}

/** Read the first line of `dir`/`name` into `out` of `cap` bytes, without its
 * newline; an empty string when the file cannot be read. */
static void get(const char *dir, const char *name, char *out, int cap)
{
    char path[4400];
    FILE *f;

    out[0] = '\0';
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    f = fopen(path, "r");
    if (!f) {
        return;
    }
    if (!fgets(out, cap, f)) {
        out[0] = '\0';
    }
    fclose(f);
    out[strcspn(out, "\n")] = '\0';
}

/** Make switch rfkill`n` of `type` with its soft and hard blocks. */
static void sw(int n, const char *type, int soft, int hard)
{
    char dir[4400];

    snprintf(dir, sizeof(dir), "%s/rfkill%d", class_dir, n);
    make_dir(dir);
    put(dir, "type", type);
    put(dir, "soft", soft ? "1\n" : "0\n");
    put(dir, "hard", hard ? "1\n" : "0\n");
}

/** The soft block of rfkill`n`: 1, 0, or -1 when it cannot be read. */
static int soft(int n)
{
    char dir[4400], text[16];

    snprintf(dir, sizeof(dir), "%s/rfkill%d", class_dir, n);
    get(dir, "soft", text, sizeof(text));
    return strcmp(text, "1") == 0 ? 1 : strcmp(text, "0") == 0 ? 0 : -1;
}

/** Whether the log holds exactly `want`, the stand-in's runs, one a line. */
static int logged(const char *want)
{
    char text[256];
    FILE *f = fopen(log_path, "r");
    size_t n = 0;

    if (f) {
        n = fread(text, 1, sizeof(text) - 1, f);
        fclose(f);
    }
    text[n] = '\0';
    return strcmp(text, want) == 0;
}

/** Run the script over the class with the stand-in first on PATH; its exit
 * status, or -1 when it did not exit. */
static int run(void)
{
    char path[4600];
    pid_t pid;
    int status;

    snprintf(path, sizeof(path), "%s:/bin:/usr/bin", bin_dir);
    pid = fork();
    if (pid == 0) {
        if (setenv("RFKILL_CLASS", class_dir, 1) != 0 || setenv("ACPI_LOCK", "", 1) != 0 ||
            setenv("RFKILL_LOG", log_path, 1) != 0 || setenv("PATH", path, 1) != 0) {
            _exit(126);
        }
        (void)dup2(2, 1);
        execl("/bin/sh", "sh", SCRIPT, (char *)NULL);
        _exit(127);
    }
    if (pid < 0) {
        return -1;
    }
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            return -1;
        }
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/** Make the class again, empty, and empty the log. */
static void fresh(void)
{
    remove_tree(class_dir);
    make_dir(class_dir);
    (void)unlink(log_path);
}

/** The Lenovo's switches, phy0 and ideapad_wlan, beside a Bluetooth one:
 * a press blocks the two Wi-Fi switches and leaves Bluetooth, and a second
 * unblocks them. */
static void check_presses(void)
{
    fresh();
    sw(0, "wlan\n", 0, 0);
    sw(1, "wlan\n", 0, 0);
    sw(2, "bluetooth\n", 0, 0);
    CHECK(run() == 0 && logged("block wifi\n"), "a press with Wi-Fi on runs rfkill block wifi");
    CHECK(soft(0) == 1 && soft(1) == 1 && soft(2) == 0, "both Wi-Fi switches blocked");
    CHECK(run() == 0 && logged("block wifi\nunblock wifi\n"),
          "a second press runs rfkill unblock wifi");
    CHECK(soft(0) == 0 && soft(1) == 0, "both Wi-Fi switches unblocked");
}

/** Switches that disagree are all blocked; a hard block does not count as
 * off, and stays. */
static void check_mixed(void)
{
    fresh();
    sw(0, "wlan\n", 1, 0);
    sw(1, "wlan\n", 0, 0);
    CHECK(run() == 0 && logged("block wifi\n"), "one switch on is Wi-Fi on");
    fresh();
    sw(0, "wlan\n", 0, 1);
    CHECK(run() == 0 && logged("block wifi\n"),
          "a hard-blocked switch with its soft block clear is on");
}

/** No Wi-Fi switch, or no switch at all: nothing is run. */
static void check_none(void)
{
    fresh();
    sw(0, "bluetooth\n", 0, 0);
    CHECK(run() == 0 && logged(""), "no Wi-Fi switch runs no rfkill");
    fresh();
    CHECK(run() == 0 && logged(""), "an empty class runs no rfkill");
}

/** Run the rows; the fixture directory is removed when they pass. */
int main(void)
{
    char fake[4400];

    class_dir = fix_path("rfkill");
    bin_dir = fix_path("bin");
    log_path = fix_path("rfkill.log");
    make_dir(bin_dir);
    put(bin_dir, "rfkill", FAKE_RFKILL);
    snprintf(fake, sizeof(fake), "%s/rfkill", bin_dir);
    if (chmod(fake, 0755) != 0) {
        printf("  FAIL: cannot make %s executable\n", fake);
        return 1;
    }
    check_presses();
    check_mixed();
    check_none();
    printf("airplane: %s\n", fails ? "FAIL" : "PASS");
    if (fails == 0) {
        fix_passed();
    }
    return fails != 0;
}
