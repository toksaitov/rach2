/*
 * brightness_hosttest.c: the brightness keys' action,
 * krffs/acpi/brightness.script, run by the host's /bin/sh over backlights of
 * its own.
 *
 * host-deps: tests/host/compat/fixdir_host.c
 * host-inputs: krffs/acpi/brightness.script
 *
 * The script reads the backlights from the directory BACKLIGHT_CLASS names,
 * and takes no lock with ACPI_LOCK empty, since a host may lack flock(1).
 * Each row makes that directory in the test's fixture directory
 * (compat/fixdir_host.c), which is kept when a row fails, with backlights
 * that have the files of /sys/class/backlight, runs the script, and reads
 * the levels it wrote. The rows check the steps of a tenth of max_brightness, a
 * step down that stops at a twentieth and a step up that stops at the
 * maximum, both at least 1 on a small range; a level below the twentieth
 * left by a step down; the direction taken from the name the script is run
 * by, as acpid runs it; a refused direction that changes nothing; and a
 * backlight whose level cannot be read failing the run while the others
 * still step.
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

#define SCRIPT "krffs/acpi/brightness.script"

/** The number of failed checks, which is the exit status. */
static int fails;

/** The backlight class, in the fixture directory. */
static const char *class_dir;

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

/** Make the backlight `name` in the class with `max` and level `level`,
 * which is written as given so that a row can give one that is no number. */
static void backlight(const char *name, long max, const char *level)
{
    char dir[4400], text[32];

    snprintf(dir, sizeof(dir), "%s/%s", class_dir, name);
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
        printf("  FAIL: cannot make %s\n", dir);
        exit(1);
    }
    snprintf(text, sizeof(text), "%ld\n", max);
    put(dir, "max_brightness", text);
    put(dir, "brightness", level);
    put(dir, "type", "raw\n");
}

/** The level of the backlight `name`, or -1 when it is not a number. */
static long level(const char *name)
{
    char path[4400], text[32], *end;
    FILE *f;
    long v;

    snprintf(path, sizeof(path), "%s/%s/brightness", class_dir, name);
    f = fopen(path, "r");
    if (!f) {
        return -1;
    }
    if (!fgets(text, sizeof(text), f)) {
        fclose(f);
        return -1;
    }
    fclose(f);
    v = strtol(text, &end, 10);
    return end != text && (*end == '\n' || *end == '\0') ? v : -1;
}

/**
 * Run the script `script` with the argument `way`, or with none when `way`
 * is NULL, over the class, its output to standard error. Returns its exit
 * status, or -1 when it did not exit.
 */
static int run(const char *script, const char *way)
{
    pid_t pid = fork();
    int status;

    if (pid == 0) {
        if (setenv("BACKLIGHT_CLASS", class_dir, 1) != 0 || setenv("ACPI_LOCK", "", 1) != 0) {
            _exit(126);
        }
        (void)dup2(2, 1);
        execl("/bin/sh", "sh", script, way, (char *)NULL);
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

/** Remove the class's backlights and make the class again, empty. */
static void fresh_class(void)
{
    remove_tree(class_dir);
    if (mkdir(class_dir, 0755) != 0) {
        printf("  FAIL: cannot make %s\n", class_dir);
        exit(1);
    }
}

/** The Lenovo's range, 937: ten steps down to the twentieth, and back. */
static void check_lenovo(void)
{
    static const long down[] = {844, 751, 658, 565, 472, 379, 286, 193, 100, 46, 46};
    static const long up[] = {139, 232, 325, 418, 511, 604, 697, 790, 883, 937, 937};
    unsigned i;
    int ok = 1;

    fresh_class();
    backlight("intel_backlight", 937, "937\n");
    for (i = 0; i < sizeof(down) / sizeof(down[0]); i++) {
        ok = ok && run(SCRIPT, "down") == 0 && level("intel_backlight") == down[i];
    }
    CHECK(ok, "down steps 937 by 93 to 100, then stops at 46");
    ok = 1;
    for (i = 0; i < sizeof(up) / sizeof(up[0]); i++) {
        ok = ok && run(SCRIPT, "up") == 0 && level("intel_backlight") == up[i];
    }
    CHECK(ok, "up steps 46 by 93 to 883, then stops at 937");
}

/** A range of 15, whose tenth and twentieth are below 1, and a level below
 * the twentieth that root wrote. */
static void check_small(void)
{
    fresh_class();
    backlight("small", 15, "2\n");
    backlight("dark", 937, "0\n");
    CHECK(run(SCRIPT, "down") == 0 && level("small") == 1, "a step of at least 1, from 2 to 1");
    CHECK(run(SCRIPT, "down") == 0 && level("small") == 1, "a floor of at least 1");
    CHECK(level("dark") == 0, "down leaves a level below the twentieth");
    CHECK(run(SCRIPT, "up") == 0 && level("small") == 2 && level("dark") == 93,
          "up steps every backlight, from below the twentieth too");
    put(class_dir, "small/brightness", "15\n");
    CHECK(run(SCRIPT, "up") == 0 && level("small") == 15, "up stops at the maximum");
}

/** Copy the script to `path`. Exits the test on a failure. */
static void copy_script(const char *path)
{
    FILE *in = fopen(SCRIPT, "r"), *out = fopen(path, "w");
    char buffer[4096];
    size_t n;
    int ok = in && out;

    while (ok && (n = fread(buffer, 1, sizeof(buffer), in)) > 0) {
        ok = fwrite(buffer, 1, n, out) == n;
    }
    ok = ok && !ferror(in);
    if (in) {
        fclose(in);
    }
    if (out && fclose(out) != 0) {
        ok = 0;
    }
    if (!ok) {
        printf("  FAIL: cannot copy " SCRIPT " to %s\n", path);
        exit(1);
    }
}

/** The direction from the name the script is run by, as acpid runs it, and
 * a direction refused. */
static void check_names(void)
{
    const char *up = fix_path("up"), *down = fix_path("down");

    copy_script(up);
    copy_script(down);
    fresh_class();
    backlight("intel_backlight", 937, "500\n");
    CHECK(run(down, NULL) == 0 && level("intel_backlight") == 407, "run as down, a step down");
    CHECK(run(up, NULL) == 0 && level("intel_backlight") == 500, "run as up, a step up");
    CHECK(run(SCRIPT, "sideways") == 2 && level("intel_backlight") == 500,
          "another direction exits 2 and changes nothing");
    CHECK(run(SCRIPT, NULL) == 2 && level("intel_backlight") == 500,
          "no direction in the name or the argument exits 2");
}

/** A backlight whose level is not a number fails the run, and the others
 * still step. */
static void check_unreadable(void)
{
    fresh_class();
    backlight("broken", 937, "x\n");
    backlight("intel_backlight", 937, "500\n");
    CHECK(run(SCRIPT, "up") == 1 && level("intel_backlight") == 593 && level("broken") == -1,
          "an unreadable level exits 1, and the other backlight steps");
}

/** Run the rows; the fixture directory is removed when they pass. */
int main(void)
{
    class_dir = fix_path("backlight");
    check_lenovo();
    check_small();
    check_names();
    check_unreadable();
    printf("brightness: %s\n", fails ? "FAIL" : "PASS");
    if (fails == 0) {
        fix_passed();
    }
    return fails != 0;
}
