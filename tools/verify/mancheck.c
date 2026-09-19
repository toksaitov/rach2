/*
 * mancheck: every program, applet and system call has a manual page.
 *
 * The inventory comes from four places and the pages from one. The programs
 * are what the manifests stage under /bin and /sbin, by name, an `exec-dir`
 * line expanded to the executable files of its directory, and what
 * `utils/programs.mk` builds; the applets are the names the built BusyBox
 * reports in the applet table its build keeps beside the binary, since the
 * guest binary does not run on the host; the system calls are the names the
 * kernel registers in its table, read from `syscall.c` and numbered by
 * `syscall.h`, and the socket operations behind `socketcall`, read from the
 * same file. The pages are `man/manN/NAME.N`, and `man/aliases` names the
 * pages a second name reaches, one `NAME SECTION TARGET` a line.
 *
 * A program needs a page or an alias in section 1 or 8, an applet the same,
 * a call one in section 2. A name the inventory has and the pages lack is
 * printed and the check fails; so is an alias whose target is not a page, and
 * a page `mandoc -T lint` warns about. `man/recovery` names the pages the
 * recovery image must carry, and each must be a page and staged by the
 * recovery manifest. The programs the test images alone stage are not in the
 * inventory: only the manifests named on the command line are read.
 *
 *   mancheck MANDIR SYSCALL_H SYSCALL_C APPLETS PROGRAMS_MK RECOVERY_MANIFEST MANIFEST...
 *
 * Exit status: 0 when every name has a page, 1 otherwise, 2 for a file that
 * cannot be read.
 */
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define NAMES 4096
#define NAME_MAX_LEN 64

struct name {
    char text[NAME_MAX_LEN];
    char section[4];   /* the section a page or alias is in */
    char target[NAME_MAX_LEN];
    const char *from;  /* where the inventory got it */
};

static struct name pages[NAMES], aliases[NAMES], wanted[NAMES];
static int npages, naliases, nwanted;
static int failures;

static void fail(const char *fmt, const char *a, const char *b)
{
    printf("mancheck: ");
    printf(fmt, a, b);
    printf("\n");
    failures++;
}

static void add(struct name *list, int *n, const char *text, const char *section,
                const char *target, const char *from)
{
    int i;

    if (strlen(text) >= NAME_MAX_LEN || *n >= NAMES) {
        fail("%s: a name too long or too many names", text, "");
        return;
    }
    for (i = 0; i < *n; i++) {
        if (strcmp(list[i].text, text) == 0 && strcmp(list[i].section, section) == 0) {
            return;
        }
    }
    strcpy(list[*n].text, text);
    strcpy(list[*n].section, section);
    strcpy(list[*n].target, target ? target : "");
    list[*n].from = from;
    (*n)++;
}

static int has_page(const char *text, const char *section)
{
    int i;

    for (i = 0; i < npages; i++) {
        if (strcmp(pages[i].text, text) == 0 && strcmp(pages[i].section, section) == 0) {
            return 1;
        }
    }
    for (i = 0; i < naliases; i++) {
        if (strcmp(aliases[i].text, text) == 0 && strcmp(aliases[i].section, section) == 0) {
            return 1;
        }
    }
    return 0;
}

/* The pages: man/manN/NAME.N, and a lint of each. */
static void read_pages(const char *mandir)
{
    const char *sections = "12345678";
    const char *s;

    for (s = sections; *s; s++) {
        char dir[512], section[4];
        DIR *d;
        struct dirent *e;

        section[0] = *s;
        section[1] = '\0';
        snprintf(dir, sizeof(dir), "%s/man%s", mandir, section);
        d = opendir(dir);
        if (!d) {
            continue;
        }
        while ((e = readdir(d)) != NULL) {
            char name[NAME_MAX_LEN], cmd[1024];
            size_t len = strlen(e->d_name);

            if (e->d_name[0] == '.' || len < 3 || e->d_name[len - 2] != '.' ||
                e->d_name[len - 1] != *s || len - 2 >= NAME_MAX_LEN) {
                continue;
            }
            memcpy(name, e->d_name, len - 2);
            name[len - 2] = '\0';
            add(pages, &npages, name, section, NULL, "page");
            snprintf(cmd, sizeof(cmd), "mandoc -T lint -W warning '%s/%s' >/dev/null 2>&1", dir,
                     e->d_name);
            if (system(cmd) != 0) {
                fail("%s/%s: mandoc -T lint found a warning", dir, e->d_name);
            }
        }
        closedir(d);
    }
}

static void read_aliases(const char *path)
{
    FILE *f = fopen(path, "r");
    char line[256];

    if (!f) {
        return; /* no aliases yet */
    }
    while (fgets(line, sizeof(line), f)) {
        char name[NAME_MAX_LEN], section[4], target[NAME_MAX_LEN];

        if (line[0] == '#' || line[0] == '\n') {
            continue;
        }
        if (sscanf(line, "%63s %3s %63s", name, section, target) != 3) {
            fail("%s: an alias line is not NAME SECTION TARGET: %s", path, line);
            continue;
        }
        add(aliases, &naliases, name, section, target, "alias");
    }
    fclose(f);
}

/* The names of a manifest's /bin and /sbin entries; an exec-dir expanded. */
static void read_manifest(const char *path)
{
    FILE *f = fopen(path, "r");
    char line[1024];

    if (!f) {
        fail("%s: cannot read the manifest", path, "");
        return;
    }
    while (fgets(line, sizeof(line), f)) {
        char kind[32], mode[32], guest[256], host[512];
        const char *base;

        if (sscanf(line, "%31s %31s %255s %511s", kind, mode, guest, host) < 3) {
            continue;
        }
        if (strcmp(kind, "exec-dir") == 0) {
            DIR *d = opendir(host);
            struct dirent *e;

            if (!d) {
                continue; /* not built here; programs.mk covers the utilities */
            }
            while ((e = readdir(d)) != NULL) {
                char full[1024];
                struct stat st;

                if (e->d_name[0] == '.') {
                    continue;
                }
                snprintf(full, sizeof(full), "%s/%s", host, e->d_name);
                if (stat(full, &st) == 0 && S_ISREG(st.st_mode) && (st.st_mode & 0111)) {
                    add(wanted, &nwanted, e->d_name, "1", NULL, path);
                }
            }
            closedir(d);
            continue;
        }
        if (strcmp(kind, "file") != 0 && strcmp(kind, "optional-file") != 0) {
            continue;
        }
        if (strncmp(guest, "/bin/", 5) == 0) {
            base = guest + 5;
        } else if (strncmp(guest, "/sbin/", 6) == 0) {
            base = guest + 6;
        } else {
            continue;
        }
        if (strchr(base, '/') || strcmp(base, "busybox") == 0) {
            continue;
        }
        add(wanted, &nwanted, base, "1", NULL, path);
    }
    fclose(f);
}

/* UTILITIES and SBIN_PROGS from programs.mk, with their continuation lines.
   The init variants are one program, /sbin/init. */
static void read_programs_mk(const char *path)
{
    FILE *f = fopen(path, "r");
    char line[1024];
    int in_list = 0;

    if (!f) {
        fail("%s: cannot read", path, "");
        return;
    }
    while (fgets(line, sizeof(line), f)) {
        char *p = line, *word;

        if (!in_list) {
            if (strncmp(line, "UTILITIES", 9) == 0 || strncmp(line, "SBIN_PROGS", 10) == 0) {
                p = strchr(line, '=');
                if (!p) {
                    continue;
                }
                p++;
                in_list = 1;
            } else {
                continue;
            }
        }
        for (word = strtok(p, " \t\n\\"); word; word = strtok(NULL, " \t\n\\")) {
            if (strncmp(word, "init-", 5) == 0) {
                add(wanted, &nwanted, "init", "1", NULL, path);
            } else if (strcmp(word, "adduser-crash") != 0) {
                add(wanted, &nwanted, word, "1", NULL, path);
            }
        }
        if (!strchr(line, '\\')) {
            in_list = 0;
        }
    }
    fclose(f);
}

/* Every quoted name in the applet table the build keeps. */
static void read_applets(const char *path)
{
    FILE *f = fopen(path, "r");
    char line[4096];

    if (!f) {
        fail("%s: cannot read the applet table; build BusyBox first", path, "");
        return;
    }
    while (fgets(line, sizeof(line), f)) {
        char *p = line;

        while ((p = strchr(p, '"')) != NULL) {
            char *q = strchr(p + 1, '"');
            char name[NAME_MAX_LEN];
            size_t len;

            if (!q) {
                break;
            }
            len = (size_t)(q - p - 1);
            if (len > 0 && len < NAME_MAX_LEN) {
                size_t i;
                int ok = 1;

                memcpy(name, p + 1, len);
                name[len] = '\0';
                for (i = 0; i < len; i++) {
                    if (!islower((unsigned char)name[i]) && !isdigit((unsigned char)name[i]) &&
                        name[i] != '_' && name[i] != '-' && name[i] != '.') {
                        ok = 0;
                    }
                }
                if (ok && strcmp(name, "busybox") != 0) {
                    add(wanted, &nwanted, name, "1", NULL, "applet");
                }
            }
            p = q + 1;
        }
    }
    fclose(f);
}

/* The calls the kernel registers, lowercased, and the socket operations. */
static void read_syscalls(const char *header, const char *source)
{
    FILE *f = fopen(source, "r");
    char line[512];

    (void)header; /* the numbers are the header's; the names suffice here */
    if (!f) {
        fail("%s: cannot read", source, "");
        return;
    }
    while (fgets(line, sizeof(line), f)) {
        char *p, *q;
        char name[NAME_MAX_LEN];
        size_t i, len;

        p = strstr(line, "_syscall_table[SYS_");
        if (p) {
            p += strlen("_syscall_table[SYS_");
            q = strchr(p, ']');
        } else if (strncmp(line, "#define SC_", 11) == 0) {
            p = line + 11;
            q = p;
            while (*q && !isspace((unsigned char)*q)) {
                q++;
            }
        } else {
            continue;
        }
        if (!q || (len = (size_t)(q - p)) == 0 || len >= NAME_MAX_LEN) {
            continue;
        }
        for (i = 0; i < len; i++) {
            name[i] = (char)tolower((unsigned char)p[i]);
        }
        name[len] = '\0';
        add(wanted, &nwanted, name, "2", NULL, source);
    }
    fclose(f);
}

/* The pages the recovery image must carry, and their staging. */
static void check_recovery(const char *listpath, const char *manifest)
{
    FILE *f = fopen(listpath, "r");
    char line[256];

    if (!f) {
        return;
    }
    while (fgets(line, sizeof(line), f)) {
        char name[NAME_MAX_LEN], section[4], want[512], mline[1024];
        FILE *m;
        int staged = 0;

        if (line[0] == '#' || sscanf(line, "%63s %3s", name, section) != 2) {
            continue;
        }
        if (!has_page(name, section)) {
            fail("recovery needs %s(%s) and there is no such page", name, section);
            continue;
        }
        snprintf(want, sizeof(want), " /usr/share/man/cat%s/%s.%s ", section, name, section);
        m = fopen(manifest, "r");
        if (!m) {
            fail("%s: cannot read the recovery manifest", manifest, "");
            break;
        }
        while (fgets(mline, sizeof(mline), m)) {
            if (strstr(mline, want)) {
                staged = 1;
            }
        }
        fclose(m);
        if (!staged) {
            fail("the recovery manifest does not stage %s(%s)", name, section);
        }
    }
    fclose(f);
}

int main(int argc, char **argv)
{
    char path[512];
    int i, programs = 0, calls = 0;

    if (argc < 8) {
        fprintf(stderr,
                "usage: mancheck MANDIR SYSCALL_H SYSCALL_C APPLETS PROGRAMS_MK "
                "RECOVERY_MANIFEST MANIFEST...\n");
        return 2;
    }
    read_pages(argv[1]);
    snprintf(path, sizeof(path), "%s/aliases", argv[1]);
    read_aliases(path);
    for (i = 0; i < naliases; i++) {
        int j, found = 0;

        for (j = 0; j < npages; j++) {
            if (strcmp(pages[j].text, aliases[i].target) == 0 &&
                strcmp(pages[j].section, aliases[i].section) == 0) {
                found = 1;
            }
        }
        if (!found) {
            fail("alias %s names no page %s in its section", aliases[i].text, aliases[i].target);
        }
    }
    read_syscalls(argv[2], argv[3]);
    read_applets(argv[4]);
    read_programs_mk(argv[5]);
    read_manifest(argv[6]);
    for (i = 7; i < argc; i++) {
        read_manifest(argv[i]);
    }
    for (i = 0; i < nwanted; i++) {
        if (strcmp(wanted[i].section, "2") == 0) {
            calls++;
            if (!has_page(wanted[i].text, "2")) {
                fail("%s has no page in section 2 (registered in %s)", wanted[i].text,
                     wanted[i].from);
            }
        } else {
            programs++;
            if (!has_page(wanted[i].text, "1") && !has_page(wanted[i].text, "8")) {
                fail("%s has no page in section 1 or 8 (from %s)", wanted[i].text,
                     wanted[i].from);
            }
        }
    }
    snprintf(path, sizeof(path), "%s/recovery", argv[1]);
    check_recovery(path, argv[6]);
    if (failures) {
        printf("mancheck: %d names without a page or a fault, over %d pages and %d aliases\n",
               failures, npages, naliases);
        return 1;
    }
    printf("mancheck: ok: %d pages and %d aliases cover %d programs and applets and %d calls\n",
           npages, naliases, programs, calls);
    return 0;
}
