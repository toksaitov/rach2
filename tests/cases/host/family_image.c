/* family_image.c: race, interruption and restaging assertions in C. */
#define _POSIX_C_SOURCE 200809L
#include "family_image.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
static int concurrent(struct fixture *f)
{
    struct fx_child a, b;
    char image[8192], meet[8192];
    const char *ae[] = {"IMAGERACE_BUILD=a", image, meet, NULL};
    const char *be[] = {"IMAGERACE_BUILD=b", image, meet, NULL};
    int started;
    if (!family_image_tree(f) || !family_image_make(f, "prog") || !fx_mock(f, "touch") ||
        !fx_mkdir(family_path(f, "meet")))
        return 0;
    snprintf(image, sizeof(image), "IMAGERACE_OUT=%s/tree/tree.krffs", f->work);
    snprintf(meet, sizeof(meet), "IMAGERACE_MEET=%s/meet", f->work);
    fx_env("FAMILY_IMAGE", "1");
    started = family_image_build(f, &a, ae) && family_image_build(f, &b, be);
    if (!started)
        return 0;
    while (!fx_poll(&a) || !fx_poll(&b)) {
        if (a.done || b.done)
            fx_text(family_path(f, "meet/release"), "");
        fx_pause();
    }
    fx_wait(f, &a);
    fx_wait(f, &b);
    family_row(f, "met",
               fx_exists(family_path(f, "meet/met.a")) && fx_exists(family_path(f, "meet/met.b")));
    family_row(f, "first", a.status == 0);
    family_row(f, "second", b.status == 0);
    family_row(f, "image",
               family_contains(family_path(f, "tree/tree.krffs"), "IMAGERACE FIRST WORDS"));
    family_row(f, "deps",
               fx_has(family_path(f, "tree/tree.krffs.d"), family_path(f, "tree/bin/prog")));
    family_row(f, "temporaries", family_image_left(f) == 0);
    return 1;
}
static int restage(struct fixture *f)
{
    int ok;
    char image[4096];
    snprintf(image, sizeof(image), "%s/tree/tree.krffs", f->work);
    if (!family_image_tree(f) || !family_image_make(f, image))
        return 0;
    family_row(f, "first", family_contains(image, "IMAGERACE FIRST WORDS"));
    ok = family_image_old(family_path(f, "tree")) &&
         fx_replace(family_path(f, "tree/prog.c"), family_path(f, "tree/prog.new"), "FIRST WORDS",
                    "SECOND WORDS") &&
         rename(family_path(f, "tree/prog.new"), family_path(f, "tree/prog.c")) == 0 &&
         family_image_make(f, image);
    family_row(f, "changed",
               ok && family_contains(image, "IMAGERACE SECOND WORDS") &&
                   !family_contains(image, "IMAGERACE FIRST WORDS"));
    family_row(f, "unchanged", family_image_make(f, image) && !strstr(f->out, "mkimage.sh"));
    ok = family_image_old(family_path(f, "tree")) &&
         fx_mkdir(family_path(f, "tree/bin/work.proof")) &&
         fx_text(family_path(f, "tree/bin/notes.log"), "") && family_image_make(f, image);
    family_row(f, "beside", ok && !strstr(f->out, "mkimage.sh"));
    ok = fx_copy(family_path(f, "tree/bin/prog"), family_path(f, "tree/bin/prog2")) &&
         family_image_make(f, image);
    family_row(f, "added", ok && strstr(f->out, "mkimage.sh"));
    ok = family_image_old(family_path(f, "tree")) &&
         remove(family_path(f, "tree/bin/prog2")) == 0 && family_image_make(f, image);
    family_row(f, "removed", ok && strstr(f->out, "mkimage.sh"));
    family_row(f, "settled", family_image_make(f, image) && !strstr(f->out, "mkimage.sh"));
    return 1;
}
static int snapshot(struct fixture *f, const char *to, const char *from)
{
    static const char *suffix[] = {"", ".d", ".programs"};
    unsigned i;
    for (i = 0; i < 3; i++) {
        char source[8192], target[8192];
        struct stat st;
        struct timespec times[2];
        snprintf(source, sizeof(source), "%s/%s/tree.krffs%s", f->work, from, suffix[i]);
        snprintf(target, sizeof(target), "%s/%s/tree.krffs%s", f->work, to, suffix[i]);
        if (stat(source, &st) || !fx_copy(source, target))
            return 0;
        times[0].tv_sec = times[1].tv_sec = st.st_mtime;
        times[0].tv_nsec = times[1].tv_nsec = 0;
        if (utimensat(AT_FDCWD, target, times, 0))
            return 0;
    }
    return 1;
}
static int clear_temporaries(struct fixture *f)
{
    DIR *dir = opendir(family_path(f, "tree"));
    struct dirent *e;
    int ok = dir != NULL;
    if (!dir)
        return 0;
    while ((e = readdir(dir))) {
        char path[8192];
        if (strncmp(e->d_name, "tree.krffs.", 11) || !strcmp(e->d_name, "tree.krffs.d") ||
            !strcmp(e->d_name, "tree.krffs.programs"))
            continue;
        snprintf(path, sizeof(path), "%s/tree/%s", f->work, e->d_name);
        ok &= fx_remove(path);
    }
    closedir(dir);
    return ok;
}
static int stopped(struct fixture *f, unsigned step)
{
    struct fx_child p;
    char directory[4096], stop[80], pid[80];
    snprintf(directory, sizeof(directory), "%s/steps.%u", f->work, step);
    if (!fx_mkdir(directory))
        return 0;
    snprintf(stop, sizeof(stop), "%u", step);
    fx_env("IMAGERACE_STEPS", directory);
    fx_env("IMAGERACE_STOP", stop);
    if (!family_image_build(f, &p, NULL))
        return 0;
    snprintf(pid, sizeof(pid), "%ld\n", (long)p.pid);
    snprintf(directory, sizeof(directory), "%s/steps.%u/pid", f->work, step);
    if (!fx_text(directory, pid))
        return 0;
    fx_wait(f, &p);
    fx_env("IMAGERACE_STEPS", NULL);
    fx_env("IMAGERACE_STOP", NULL);
    return 1;
}
static int counted(struct fixture *f, unsigned *steps)
{
    unsigned flags = 0, moves = 0;
    for (*steps = 0;;) {
        char path[8192], *line;
        snprintf(path, sizeof(path), "%s/steps.0/%u/command", f->work, *steps + 1);
        if (!fx_exists(path))
            break;
        (*steps)++;
        line = fx_read(path);
        if (!strncmp(line, "dd ", 3))
            flags |= 1;
        if (!strncmp(line, "touch ", 6))
            flags |= 2;
        if (!strncmp(line, "mv ", 3))
            moves++;
        free(line);
    }
    return flags == 3 && moves >= 3;
}
static int interrupted(struct fixture *f)
{
    const char *commands[] = {"dd", "sed", "grep", "mv", "touch", NULL};
    char image[4096];
    snprintf(image, sizeof(image), "%s/tree/tree.krffs", f->work);
    unsigned i, steps;
    int status = 1, left = 1, matched = 1, current = 1;
    if (!family_image_tree(f) || !family_image_make(f, image) || !snapshot(f, "old", "tree") ||
        !fx_write(family_path(f, "tree/bin/prog2"), "#!/bin/sh\necho IMAGERACE SECOND PROGRAM\n",
                  40, 0755))
        return 0;
    for (i = 0; commands[i]; i++)
        if (!fx_mock(f, commands[i]))
            return 0;
    fx_env("IMAGERACE_OUT", image);
    fx_env("FAMILY_IMAGE", "1");
    if (!stopped(f, 0) || f->status || !family_contains(image, "IMAGERACE SECOND PROGRAM") ||
        !snapshot(f, "new", "tree"))
        return 0;
    family_row(f, "counted", counted(f, &steps));
    for (i = 1; i <= steps; i++) {
        char path[8192];
        if (!clear_temporaries(f) || !snapshot(f, "tree", "old") || !stopped(f, i))
            return 0;
        snprintf(path, sizeof(path), "%s/steps.%u/%u/stopped", f->work, i, i);
        status &= f->status != 0 && fx_exists(path);
        left &= family_image_left(f) == 0;
        if (family_contains(image, "IMAGERACE SECOND PROGRAM"))
            matched &=
                fx_has(family_path(f, "tree/tree.krffs.d"), family_path(f, "tree/bin/prog2")) &&
                fx_has(family_path(f, "tree/tree.krffs.programs"), " prog2");
        current &=
            family_image_make(f, image) && family_contains(image, "IMAGERACE FIRST WORDS") &&
            family_contains(image, "IMAGERACE SECOND PROGRAM") &&
            fx_same(family_path(f, "tree/tree.krffs.d"), family_path(f, "new/tree.krffs.d")) &&
            fx_same(family_path(f, "tree/tree.krffs.programs"),
                    family_path(f, "new/tree.krffs.programs"));
    }
    family_row(f, "status", status);
    family_row(f, "temporaries", left);
    family_row(f, "matched", matched);
    family_row(f, "current", current);
    return 1;
}
int family_image(struct fixture *f, const char *id)
{
    fx_env("MAKEFLAGS", "");
    if (!strcmp(id, "host.imagerace.concurrent"))
        return concurrent(f);
    if (!strcmp(id, "host.imagerace.interrupted"))
        return interrupted(f);
    if (!strcmp(id, "host.imagerace.restage"))
        return restage(f);
    if (!strcmp(id, "host.imagerace.prerequisites"))
        return family_image_prerequisites(f);
    if (!strcmp(id, "host.imagerace.probe"))
        return family_image_probe(f);
    return 0;
}
