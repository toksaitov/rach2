/*
 * mancheck: check that every program, applet and system call has a manual
 * page.
 *
 * The inventory comes from four places and the pages from one. The programs
 * are what the manifests stage under /bin and /sbin, by name, an `exec-dir`
 * line expanded to the executable files of its directory, and what
 * `utils/programs.mk` builds; the applets are the names in the applet table
 * the BusyBox build keeps beside the binary, since the guest binary does not
 * run on the host; the system calls are the names an architecture's table
 * registers, read from its `systab.c` and numbered by its `sysnr.h`, and the
 * socket operations behind `socketcall`, read from `syscall_net.c`. The pages
 * are `man/manN/NAME.N`, and `man/aliases` lists the pages a second name
 * reaches, one `NAME SECTION TARGET` a line.
 *
 * A program needs a page or an alias in section 1 or 8, an applet the same,
 * a call one in section 2. A name the inventory has and the pages lack is
 * printed and the check fails; so is an alias whose target is not a page, and
 * a page `mandoc -T lint` warns about. `man/recovery` lists the pages the
 * recovery image must carry, and each must be a page and staged by the
 * recovery manifest. The programs the test images alone stage are not in the
 * inventory: only the manifests named on the command line are read, the
 * recovery manifest among them.
 *
 *   mancheck MANDIR SYSNR_H SYSTAB_C[:SYSTAB_C...] APPLETS PROGRAMS_MK
 *            RECOVERY_MANIFEST MANIFEST...
 *
 * SYSTAB_C is a colon-separated list of the files to read the calls and the
 * socket operations from, and SYSNR_H gives each call its number. The exit
 * status is 0 when every name has a page, 1 when a name has none or a file
 * cannot be read, and 2 for a wrong usage.
 *
 *   mancheck --calls MANDIR I686_SYSNR_H I686_SYSTAB_C X86_64_SYSNR_H
 *            X86_64_SYSTAB_C
 *
 * reads no manifest and runs no lint, and prints the Markdown of
 * `rach2/syscalls.md`: the calls the two tables register, one row a call in
 * name order, with its number in each table and a link to its page relative
 * to a directory beside MANDIR. It prints nothing and exits 1 when a call has
 * no page or no number, and exits 2 for a wrong number of arguments.
 */
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define NAMES        4096
#define NAME_MAX_LEN 64

/** A page, an alias, or a name the inventory needs a page for. */
struct name {
    char text[NAME_MAX_LEN];
    char section[4]; /* the section a page or alias is in */
    /* an alias's page; empty otherwise */
    char target[NAME_MAX_LEN];
    const char *from; /* where the inventory got it */
    long number;      /* a call's number, -1 for none */
};

static struct name pages[NAMES], aliases[NAMES], wanted[NAMES];
static int npages, naliases, nwanted;
static int failures;
static int programs_listed, applets_read; /* what each inventory source gave */

/**
 * Prints a finding, `fmt` formatted with the two strings `a` and `b`, on
 * standard error and counts it.
 */
static void fail(const char *fmt, const char *a, const char *b)
{
    fprintf(stderr, "mancheck: ");
    fprintf(stderr, fmt, a, b);
    fprintf(stderr, "\n");
    failures++;
}

/**
 * Adds a name to a list; returns 0 when the list had it already in the same
 * section, and 1 otherwise. A name too long, or a list full, is a finding and
 * is not added. `from` is kept by reference and must outlive the program's
 * reports.
 */
static int add(struct name *list, int *n, const char *text, const char *section, const char *target,
               const char *from)
{
    int i;

    if (strlen(text) >= NAME_MAX_LEN || *n >= NAMES) {
        fail("%s: a name too long or too many names", text, "");
        return 1;
    }
    for (i = 0; i < *n; i++) {
        if (strcmp(list[i].text, text) == 0 && strcmp(list[i].section, section) == 0) {
            return 0;
        }
    }
    strcpy(list[*n].text, text);
    strcpy(list[*n].section, section);
    strcpy(list[*n].target, target ? target : "");
    list[*n].from = from;
    list[*n].number = -1;
    (*n)++;
    return 1;
}

/**
 * The page `text` is read on in `section`: its own name, or the target of its
 * alias; NULL when it has neither.
 */
static const char *page_of(const char *text, const char *section)
{
    int i;

    for (i = 0; i < npages; i++) {
        if (strcmp(pages[i].text, text) == 0 && strcmp(pages[i].section, section) == 0) {
            return pages[i].text;
        }
    }
    for (i = 0; i < naliases; i++) {
        if (strcmp(aliases[i].text, text) == 0 && strcmp(aliases[i].section, section) == 0) {
            return aliases[i].target;
        }
    }
    return NULL;
}

/** Whether `text` has a page or an alias in `section`. */
static int has_page(const char *text, const char *section)
{
    return page_of(text, section) != NULL;
}

/**
 * Reads the pages, `MANDIR/manN/NAME.N`, and lints each with mandoc when
 * `lint` is set.
 */
static void read_pages(const char *mandir, int lint)
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
            if (!lint) {
                continue;
            }
            snprintf(cmd, sizeof(cmd), "mandoc -T lint -W warning '%s/%s' >/dev/null 2>&1", dir,
                     e->d_name);
            if (system(cmd) != 0) {
                fail("%s/%s: mandoc -T lint found a warning", dir, e->d_name);
            }
        }
        closedir(d);
    }
}

/**
 * Reads the aliases file at `path`, if there is one. A malformed line, an
 * alias that is also a page, and an alias listed twice are findings.
 */
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
        if (has_page(name, section)) {
            fail("alias %s is also a page of section %s", name, section);
        }
        if (!add(aliases, &naliases, name, section, target, "alias")) {
            fail("alias %s is listed twice in section %s", name, section);
        }
    }
    fclose(f);
}

/**
 * Adds the programs a manifest stages: the names of its `file` and
 * `optional-file` entries directly under /bin and /sbin, and the executable
 * files of each `exec-dir` host directory that exists.
 */
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
        int fields;

        if (line[0] == '#') {
            continue;
        }
        fields = sscanf(line, "%31s %31s %255s %511s", kind, mode, guest, host);
        if (fields < 3) {
            continue;
        }
        if (strcmp(kind, "exec-dir") == 0) {
            DIR *d;
            struct dirent *e;

            if (fields < 4) {
                fail("%s: an exec-dir entry without a host directory: %s", path, guest);
                continue;
            }
            d = opendir(host);
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
        if (strchr(base, '/')) {
            continue;
        }
        add(wanted, &nwanted, base, "1", NULL, path);
    }
    fclose(f);
}

/**
 * Adds the programs UTILITIES and SBIN_PROGS of programs.mk list, with their
 * continuation lines. The init variants are one program, /sbin/init, and
 * adduser-crash, a build of adduser for a probe, has no page.
 */
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
        int continued;

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
        continued = strchr(line, '\\') != NULL; /* strtok cuts the line below */
        for (word = strtok(p, " \t\n\\"); word; word = strtok(NULL, " \t\n\\")) {
            if (strncmp(word, "init-", 5) == 0) {
                add(wanted, &nwanted, "init", "1", NULL, path);
            } else if (strcmp(word, "adduser-crash") != 0) {
                add(wanted, &nwanted, word, "1", NULL, path);
                programs_listed++;
            }
        }
        if (!continued) {
            in_list = 0;
        }
    }
    fclose(f);
}

/**
 * Adds every quoted name in the applet table the BusyBox build keeps that is
 * made of lower-case letters, digits and `_`, `-`, `.` or `[`.
 */
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
                        name[i] != '_' && name[i] != '-' && name[i] != '.' && name[i] != '[') {
                        ok = 0;
                    }
                }
                if (ok) {
                    add(wanted, &nwanted, name, "1", NULL, "applet");
                    applets_read++;
                }
            }
            p = q + 1;
        }
    }
    fclose(f);
}

/** The number `#define MACRO NUMBER` gives in `header`, or -1 for none. */
static long header_number(const char *header, const char *macro)
{
    FILE *f = fopen(header, "r");
    char line[512];
    long number = -1;

    if (!f) {
        return -1;
    }
    while (number < 0 && fgets(line, sizeof(line), f)) {
        char name[NAME_MAX_LEN];
        long value;

        if (sscanf(line, "#define %63s %ld", name, &value) == 2 && strcmp(name, macro) == 0) {
            number = value;
        }
    }
    fclose(f);
    return number;
}

/**
 * Adds to `list` the calls one source file registers, each
 * `table[SYS_NAME]` as `name` with the number `header` gives `SYS_NAME`, and
 * the socket operations it defines, each `#define SC_NAME NUMBER` as `name`
 * with its number. A registered call the header gives no number for is a
 * finding. `source` is kept by reference by `add`.
 */
static void read_syscalls_from(const char *header, const char *source, struct name *list, int *n)
{
    FILE *f = fopen(source, "r");
    char line[512];

    if (!f) {
        fail("%s: cannot read", source, "");
        return;
    }
    while (fgets(line, sizeof(line), f)) {
        char *p, *q;
        char name[NAME_MAX_LEN], macro[NAME_MAX_LEN + 4];
        size_t i, len;
        long number = -1;
        int before = *n;

        p = strstr(line, "table[SYS_");
        if (p) {
            p += strlen("table[SYS_");
            q = strchr(p, ']');
        } else if (strncmp(line, "#define SC_", 11) == 0) {
            /* `SC_BE16(p)` and the other helper macros are not operations. */
            const char *r;

            p = line + 11;
            q = p;
            while (*q && !isspace((unsigned char)*q)) {
                q++;
            }
            r = q;
            while (isspace((unsigned char)*r)) {
                r++;
            }
            if ((strchr(p, '(') != NULL && strchr(p, '(') < q) || !isdigit((unsigned char)*r)) {
                continue;
            }
            number = (long)strtoul(r, (char **)&r, 0);
            if (!isspace((unsigned char)*r)) {
                continue; /* a constant with a suffix, such as SC_ID_KEEP */
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
        if (number < 0) {
            memcpy(macro, "SYS_", 4);
            memcpy(macro + 4, p, len);
            macro[len + 4] = '\0';
            number = header_number(header, macro);
            if (number < 0) {
                fail("%s: no number for %s", header, macro);
            }
        }
        add(list, n, name, "2", NULL, source);
        if (*n > before) {
            list[*n - 1].number = number;
        }
    }
    fclose(f);
}

/**
 * Adds to `list` the calls and the socket operations of each file `sources`
 * lists, joined by colons: an architecture's systab.c registers the calls and
 * syscall_net.c defines the socket operations. `header` is the
 * architecture's sysnr.h.
 *
 * The path each call is recorded as coming from is printed when the call has
 * no page, after this function has returned, so the paths are kept in static
 * storage for the program's life. Up to eight paths of under 512 bytes are
 * read; more, or a longer one, is a finding.
 */
static void read_syscalls(const char *header, const char *sources, struct name *list, int *n)
{
    static char paths[8][512];
    static int npaths;
    const char *p = sources;

    while (*p != '\0') {
        const char *q = strchr(p, ':');
        size_t len = q ? (size_t)(q - p) : strlen(p);

        if (len >= sizeof(paths[0])) {
            fail("%s: a source path is longer than this reads", sources, "");
            return;
        }
        if (npaths == (int)(sizeof(paths) / sizeof(paths[0]))) {
            fail("%s: more source paths than this reads", sources, "");
            return;
        }
        memcpy(paths[npaths], p, len);
        paths[npaths][len] = '\0';
        read_syscalls_from(header, paths[npaths], list, n);
        npaths++;
        p = q ? q + 1 : p + len;
    }
}

/**
 * Checks each page the list at `listpath` names: it must be a page or an
 * alias, and `manifest` must stage it with an active `file` line from
 * build/man/catN/NAME.N to /usr/share/man/catN/NAME.N. A commented line, or
 * one that stages another file, does not count.
 */
static void check_recovery(const char *listpath, const char *manifest)
{
    FILE *f = fopen(listpath, "r");
    char line[256];

    if (!f) {
        fail("%s: cannot read the recovery page list", listpath, "");
        return;
    }
    while (fgets(line, sizeof(line), f)) {
        char name[NAME_MAX_LEN], section[4], want[512], from[512], mline[1024];
        FILE *m;
        int staged = 0;

        if (line[0] == '#' || sscanf(line, "%63s %3s", name, section) != 2) {
            continue;
        }
        if (!has_page(name, section)) {
            fail("recovery needs %s(%s) and there is no such page", name, section);
            continue;
        }
        snprintf(want, sizeof(want), "/usr/share/man/cat%s/%s.%s", section, name, section);
        snprintf(from, sizeof(from), "build/man/cat%s/%s.%s", section, name, section);
        m = fopen(manifest, "r");
        if (!m) {
            fail("%s: cannot read the recovery manifest", manifest, "");
            break;
        }
        while (fgets(mline, sizeof(mline), m)) {
            char kind[32], mode[32], guest[256], host[512];

            if (mline[0] == '#' ||
                sscanf(mline, "%31s %31s %255s %511s", kind, mode, guest, host) != 4) {
                continue;
            }
            if (strcmp(kind, "file") == 0 && strcmp(guest, want) == 0 && strcmp(host, from) == 0) {
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

static struct name calls32[NAMES], calls64[NAMES]; /* the i686 and x86-64 tables */
static int ncalls32, ncalls64;

/** The number of `text` in the `n` names of `list`, or -1 when it is absent. */
static long number_in(const struct name *list, int n, const char *text)
{
    int i;

    for (i = 0; i < n; i++) {
        if (strcmp(list[i].text, text) == 0) {
            return list[i].number;
        }
    }
    return -1;
}

/** The `qsort` comparison of two names by their text. */
static int by_text(const void *a, const void *b)
{
    return strcmp(((const struct name *)a)->text, ((const struct name *)b)->text);
}

/** Prints a table cell of a call's number, `none` for a table without it. */
static void print_number(long number)
{
    if (number < 0) {
        printf(" none |");
    } else {
        printf(" %ld |", number);
    }
}

/**
 * The `--calls` mode: prints the Markdown table of the calls both tables
 * register, one row a name, only when every call has a number and a page.
 * Returns 0, 1 after a finding, or 2 for a wrong number of arguments.
 */
static int print_calls(int argc, char **argv)
{
    char path[512];
    int i;

    if (argc != 7) {
        fprintf(stderr, "usage: mancheck --calls MANDIR I686_SYSNR_H I686_SYSTAB_C "
                        "X86_64_SYSNR_H X86_64_SYSTAB_C\n");
        return 2;
    }
    read_pages(argv[2], 0);
    snprintf(path, sizeof(path), "%s/aliases", argv[2]);
    read_aliases(path);
    read_syscalls(argv[3], argv[4], calls32, &ncalls32);
    read_syscalls(argv[5], argv[6], calls64, &ncalls64);
    if (ncalls32 == 0 || ncalls64 == 0) {
        fail("%s or %s: no registered call was read", argv[4], argv[6]);
    }
    for (i = 0; i < ncalls32; i++) {
        add(wanted, &nwanted, calls32[i].text, "2", NULL, calls32[i].from);
    }
    for (i = 0; i < ncalls64; i++) {
        add(wanted, &nwanted, calls64[i].text, "2", NULL, calls64[i].from);
    }
    qsort(wanted, (size_t)nwanted, sizeof(wanted[0]), by_text);
    for (i = 0; i < nwanted; i++) {
        if (!has_page(wanted[i].text, "2")) {
            fail("%s has no page in section 2 (registered in %s)", wanted[i].text, wanted[i].from);
        }
    }
    if (failures) {
        fprintf(stderr, "mancheck: %d faults, and no table written\n", failures);
        return 1;
    }
    printf("# The system calls\n\n"
           "This table lists the calls the two kernels register, one row a call, in name "
           "order. A name is the call's constant in `sysnr.h` without its `SYS_` prefix, in "
           "lower case. A number is the one a program puts in `eax` before `int 0x80` on "
           "i686, or in `rax` before `syscall` on x86-64, and is Linux's number for the "
           "call on that architecture. `none` marks a call one kernel "
           "does not register, and a number with no handler returns `ENOSYS`. The i686 "
           "kernel registers %d calls and the x86-64 kernel %d. On i686 the socket "
           "operations, `accept` among them, are also reached through `socketcall`, whose "
           "page lists them. [syscalls(2)](../%s/man2/syscalls.2) gives the registers that "
           "hold the arguments, and each call's page says where the call differs from "
           "Linux.\n\n",
           ncalls32, ncalls64, argv[2]);
    printf("`make syscalls` writes this file from `%s` and `%s`, with the numbers in `%s` "
           "and `%s`, and `tests/run style` fails when the file differs from what it "
           "writes.\n\n",
           argv[4], argv[6], argv[3], argv[5]);
    printf("| Call | i386 | x86-64 | Page |\n|---|--:|--:|---|\n");
    for (i = 0; i < nwanted; i++) {
        const char *page = page_of(wanted[i].text, "2");

        printf("| `%s` |", wanted[i].text);
        print_number(number_in(calls32, ncalls32, wanted[i].text));
        print_number(number_in(calls64, ncalls64, wanted[i].text));
        printf(" [%s(2)](../%s/man2/%s.2) |\n", page, argv[2], page);
    }
    return 0;
}

/**
 * Reads the pages and the inventory and reports each name without a page, or
 * run the `--calls` mode.
 *
 * An inventory source that gave no name at all is a finding too: it is a
 * wrong file, since a directory given in place of a file opens and reads as
 * empty.
 */
int main(int argc, char **argv)
{
    char path[512];
    int i, programs = 0, calls = 0;

    if (argc > 1 && strcmp(argv[1], "--calls") == 0) {
        return print_calls(argc, argv);
    }
    if (argc < 8) {
        fprintf(stderr, "usage: mancheck MANDIR SYSNR_H SYSTAB_C[:SYSTAB_C...] APPLETS PROGRAMS_MK "
                        "RECOVERY_MANIFEST MANIFEST...\n");
        return 2;
    }
    read_pages(argv[1], 1);
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
    read_syscalls(argv[2], argv[3], wanted, &nwanted);
    read_applets(argv[4]);
    read_programs_mk(argv[5]);
    read_manifest(argv[6]);
    for (i = 7; i < argc; i++) {
        read_manifest(argv[i]);
    }
    if (applets_read == 0) {
        fail("%s: no applet name was read from the table", argv[4], "");
    }
    if (programs_listed == 0) {
        fail("%s: no utility was read from the program lists", argv[5], "");
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
                fail("%s has no page in section 1 or 8 (from %s)", wanted[i].text, wanted[i].from);
            }
        }
    }
    snprintf(path, sizeof(path), "%s/recovery", argv[1]);
    check_recovery(path, argv[6]);
    if (calls == 0) {
        fail("%s: no registered call was read", argv[3], "");
    }
    if (failures) {
        fprintf(stderr,
                "mancheck: %d names without a page or a fault, over %d pages and %d aliases\n",
                failures, npages, naliases);
        return 1;
    }
    printf("mancheck: ok: %d pages and %d aliases cover %d programs and applets and %d calls\n",
           npages, naliases, programs, calls);
    return 0;
}
