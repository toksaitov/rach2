/* host_image_accounts.c: run the installer's account merge on fixtures. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "host_image.h"

/* An account file left NULL is not written. */
struct accounts {
    const char *passwd, *group, *shadow;
};

/* One merge: the image's files, the machine's before and after, the scope
 * merge_accounts is given and what it prints. */
struct merge_row {
    const char *row;
    struct accounts image, before, after;
    const char *scope, *printed;
};

#define IMAGE_PASSWD                                                            \
    "root:x:0:0:root:/root:/bin/sh\ntest:x:1000:1000:test:/home/test:/bin/sh\n" \
    "sshd:x:74:74::/var/empty:/usr/bin/false\ndemo:x:1002:1002:demo:/home/demo:/bin/sh\n"
#define IMAGE_GROUP  "root:x:0:\ntest:x:1000:\nsshd:x:74:\n"
#define IMAGE_SHADOW "root:*:0:0:99999:7:::\ntest:*:0:0:99999:7:::\nsshd:!:0:0:99999:7:::\n"
#define MACHINE_PASSWD \
    "root:x:0:0:root:/root:/bin/sh\nowner:x:1001:1001:owner:/home/owner:/bin/sh\n"
#define MACHINE_GROUP  "root:x:0:\nowner:x:1001:\n"
#define MACHINE_SHADOW "root:$6$a:0:0:99999:7:::\nowner:$6$b:0:0:99999:7:::\n"
#define SSHD_PASSWD    "sshd:x:74:74::/var/empty:/usr/bin/false\n"
#define SSHD_GROUP     "sshd:x:74:\n"
#define SSHD_SHADOW    "sshd:!:0:0:99999:7:::\n"
#define SSHD_ADDED     "added sshd 74 74 /var/empty\n"
#define NOBODY_PASSWD  "nobody:x:65534:65534::/nonexistent:/usr/sbin/nologin\n"
#define NOBODY_GROUP   "nogroup:x:65534:\n"
#define NOBODY_SHADOW  "nobody:*:0:0:99999:7:::\n"

/* The GNU root's desktop account and the Lenovo's accounts before the
 * update that dropped it. */
#define GNU_PASSWD                                                                  \
    "root:x:0:0:root:/root:/bin/bash\ntest:x:1000:1000:test:/home/test:/bin/bash\n" \
    "desktop:x:1001:1001:Desktop:/home/desktop:/bin/bash\n"
#define GNU_GROUP  "root:x:0:\ntest:x:1000:\ndesktop:x:1001:\nvideo:x:44:desktop,test\n"
#define GNU_SHADOW "root:$6$x:0:0:99999:7:::\ntest:$6$y:0:0:99999:7:::\ndesktop:!:::::::\n"
#define LENOVO_PASSWD                                                             \
    "root:x:0:0:root:/root:/bin/bash\nowner:x:1000:1000::/home/owner:/bin/bash\n" \
    "test:x:60000:60000::/home/test:/bin/bash\n"
#define LENOVO_GROUP  "root:x:0:\nowner:x:1000:\ntest:x:60000:\nvideo:x:44:owner\n"
#define LENOVO_SHADOW "root:$6$r:::::::\nowner:$6$o:::::::\ntest:!:::::::\n"

static const struct merge_row rows[] = {
    {"append only missing system accounts",
     {IMAGE_PASSWD, IMAGE_GROUP, IMAGE_SHADOW},
     {MACHINE_PASSWD, MACHINE_GROUP, MACHINE_SHADOW},
     {MACHINE_PASSWD SSHD_PASSWD, MACHINE_GROUP SSHD_GROUP, MACHINE_SHADOW SSHD_SHADOW},
     "system",
     SSHD_ADDED},
    {"a second run adds nothing",
     {IMAGE_PASSWD, IMAGE_GROUP, IMAGE_SHADOW},
     {MACHINE_PASSWD SSHD_PASSWD, MACHINE_GROUP SSHD_GROUP, MACHINE_SHADOW SSHD_SHADOW},
     {MACHINE_PASSWD SSHD_PASSWD, MACHINE_GROUP SSHD_GROUP, MACHINE_SHADOW SSHD_SHADOW},
     "system",
     ""},
    {"an ID the machine has, with a leading zero, moves to the next free one",
     {IMAGE_PASSWD, IMAGE_GROUP, IMAGE_SHADOW},
     {MACHINE_PASSWD "other:x:074:74::/:/bin/sh\n", MACHINE_GROUP, MACHINE_SHADOW},
     {MACHINE_PASSWD "other:x:074:74::/:/bin/sh\nsshd:x:75:74::/var/empty:/usr/bin/false\n",
      MACHINE_GROUP SSHD_GROUP, MACHINE_SHADOW SSHD_SHADOW},
     "system",
     "uid 74 75\nadded sshd 75 74 /var/empty\n"},
    {"an account name the machine has is not added",
     {IMAGE_PASSWD, IMAGE_GROUP, IMAGE_SHADOW},
     {MACHINE_PASSWD "sshd:x:80:80::/:/bin/sh\n", MACHINE_GROUP, MACHINE_SHADOW},
     {MACHINE_PASSWD "sshd:x:80:80::/:/bin/sh\n", MACHINE_GROUP SSHD_GROUP, MACHINE_SHADOW},
     "system",
     ""},
    {"no image account files",
     {NULL, NULL, NULL},
     {MACHINE_PASSWD, MACHINE_GROUP, MACHINE_SHADOW},
     {MACHINE_PASSWD, MACHINE_GROUP, MACHINE_SHADOW},
     "system",
     ""},
    {"a name twice is taken once and a shared ID moves the second name",
     {IMAGE_PASSWD SSHD_PASSWD "sshd2:x:74:74::/:/bin/sh\n", IMAGE_GROUP SSHD_GROUP,
      IMAGE_SHADOW SSHD_SHADOW},
     {MACHINE_PASSWD, MACHINE_GROUP, MACHINE_SHADOW},
     {MACHINE_PASSWD SSHD_PASSWD "sshd2:x:75:74::/:/bin/sh\n", MACHINE_GROUP SSHD_GROUP,
      MACHINE_SHADOW SSHD_SHADOW},
     "system",
     SSHD_ADDED "added sshd2 75 74 /\n"},
    {"keep a final line without newline whole",
     {IMAGE_PASSWD, IMAGE_GROUP, IMAGE_SHADOW},
     {"root:x:0:0:root:/root:/bin/sh\nowner:x:1001:1001:owner:/home/owner:/bin/sh", MACHINE_GROUP,
      MACHINE_SHADOW},
     {MACHINE_PASSWD SSHD_PASSWD, MACHINE_GROUP SSHD_GROUP, MACHINE_SHADOW SSHD_SHADOW},
     "system",
     SSHD_ADDED},
    {"empty destination group gets system groups",
     {IMAGE_PASSWD, IMAGE_GROUP, IMAGE_SHADOW},
     {MACHINE_PASSWD, "", MACHINE_SHADOW},
     {MACHINE_PASSWD SSHD_PASSWD, "root:x:0:\n" SSHD_GROUP, MACHINE_SHADOW SSHD_SHADOW},
     "system",
     SSHD_ADDED},
    {"missing image shadow line leaves destination shadow",
     {IMAGE_PASSWD, IMAGE_GROUP, "root:*:0:0:99999:7:::\n"},
     {MACHINE_PASSWD, MACHINE_GROUP, MACHINE_SHADOW},
     {MACHINE_PASSWD SSHD_PASSWD, MACHINE_GROUP SSHD_GROUP, MACHINE_SHADOW},
     "system",
     SSHD_ADDED},
    {"system group without a new account",
     {IMAGE_PASSWD, IMAGE_GROUP "crontab:x:63:\n", IMAGE_SHADOW},
     {MACHINE_PASSWD SSHD_PASSWD, MACHINE_GROUP SSHD_GROUP, MACHINE_SHADOW SSHD_SHADOW},
     {MACHINE_PASSWD SSHD_PASSWD, MACHINE_GROUP SSHD_GROUP "crontab:x:63:\n",
      MACHINE_SHADOW SSHD_SHADOW},
     "system",
     ""},
    {"existing system accounts gain missing image shadow lines",
     {IMAGE_PASSWD "nobody:x:65534:65534::/:/bin/false\n", IMAGE_GROUP,
      IMAGE_SHADOW "nobody:!:::::::\n"},
     {MACHINE_PASSWD SSHD_PASSWD "nobody:x:65534:65534::/:/bin/false\n", MACHINE_GROUP SSHD_GROUP,
      MACHINE_SHADOW},
     {MACHINE_PASSWD SSHD_PASSWD "nobody:x:65534:65534::/:/bin/false\n", MACHINE_GROUP SSHD_GROUP,
      MACHINE_SHADOW SSHD_SHADOW "nobody:!:::::::\n"},
     "system",
     ""},
    {"image nobody and nogroup of ID 65534 are appended with their shadow line",
     {IMAGE_PASSWD NOBODY_PASSWD, IMAGE_GROUP NOBODY_GROUP, IMAGE_SHADOW NOBODY_SHADOW},
     {MACHINE_PASSWD, MACHINE_GROUP, MACHINE_SHADOW},
     {MACHINE_PASSWD SSHD_PASSWD NOBODY_PASSWD, MACHINE_GROUP SSHD_GROUP NOBODY_GROUP,
      MACHINE_SHADOW SSHD_SHADOW NOBODY_SHADOW},
     "system",
     SSHD_ADDED "added nobody 65534 65534 /nonexistent\n"},
    {"an update adds the image's ordinary account and its system group members",
     {GNU_PASSWD, GNU_GROUP, GNU_SHADOW},
     {LENOVO_PASSWD, LENOVO_GROUP, LENOVO_SHADOW},
     {LENOVO_PASSWD "desktop:x:1001:1001:Desktop:/home/desktop:/bin/bash\n",
      "root:x:0:\nowner:x:1000:\ntest:x:60000:\nvideo:x:44:owner,desktop,test\ndesktop:x:1001:\n",
      LENOVO_SHADOW "desktop:!:::::::\n"},
     "all",
     "added desktop 1001 1001 /home/desktop\n"},
    {"an update moves the IDs the machine has and locks the image's passwords",
     {GNU_PASSWD, GNU_GROUP, GNU_SHADOW},
     {MACHINE_PASSWD, MACHINE_GROUP, MACHINE_SHADOW},
     {MACHINE_PASSWD "test:x:1000:1000:test:/home/test:/bin/bash\n"
                     "desktop:x:1002:1002:Desktop:/home/desktop:/bin/bash\n",
      MACHINE_GROUP "test:x:1000:\ndesktop:x:1002:\nvideo:x:44:desktop,test\n",
      MACHINE_SHADOW "test:!$6$y:0:0:99999:7:::\ndesktop:!:::::::\n"},
     "all",
     "gid 1001 1002\nadded test 1000 1000 /home/test\nuid 1001 1002\n"
     "added desktop 1002 1002 /home/desktop\n"},
};

/** Copy the complete product function, rejecting a missing closing line. */
static char *function(const char *source, const char *name)
{
    char heading[128];
    const char *begin, *end;
    snprintf(heading, sizeof(heading), "\n%s() {\n", name);
    begin = strstr(source, heading);
    if (begin == NULL || (end = strstr(begin + 1, "\n}\n")) == NULL)
        return NULL;
    return strndup(begin + 1, (size_t)(end + 3 - (begin + 1)));
}

/** Append exact account records for one expected fixture. */
static char *append(const char *a, const char *b)
{
    size_t n = strlen(a), m = strlen(b);
    char *p = malloc(n + m + 1);
    if (p != NULL) {
        memcpy(p, a, n);
        memcpy(p + n, b, m + 1);
    }
    return p;
}

static int account_files(struct image_check *t, const char *directory, const struct accounts *a)
{
    const char *values[] = {a->passwd, a->group, a->shadow};
    const char *names[] = {"passwd", "group", "shadow"};
    char path[128];
    unsigned i;
    snprintf(path, sizeof(path), "%s/etc", directory);
    if (!image_directory(t, path, 1))
        return 0;
    for (i = 0; i < 3; i++) {
        snprintf(path, sizeof(path), "%s/etc/%s", directory, names[i]);
        if (values[i] != NULL && !image_text(t, path, values[i]))
            return 0;
    }
    return 1;
}

/** Run the product operation in SCOPE; the controller judges what it prints
 * and, after, its files. */
static int merge(struct image_check *t, const char *scope, const char *printed)
{
    char functions[1024], image_path[1024], dest[1024];
    const char *argv[] = {"/bin/sh", functions, image_path, dest, scope, NULL};
    snprintf(functions, sizeof(functions), "%s/functions", t->dir);
    snprintf(image_path, sizeof(image_path), "%s/image", t->dir);
    snprintf(dest, sizeof(dest), "%s/dest", t->dir);
    return image_expect(t, argv, printed);
}

static int same_accounts(struct image_check *t, const struct accounts *want)
{
    const char *values[] = {want->passwd, want->group, want->shadow};
    const char *names[] = {"passwd", "group", "shadow"};
    char path[1024];
    const char *argv[] = {IMAGE_FIXTURE, "read", path, NULL};
    unsigned i;
    int ok = 1;
    for (i = 0; i < 3; i++) {
        snprintf(path, sizeof(path), "%s/dest/etc/%s", t->dir, names[i]);
        if (!image_expect(t, argv, values[i]))
            ok = 0;
    }
    return ok;
}

static int source_functions(struct image_check *t)
{
    const char *prepared = etude_artifact(t->u.c, "host-source-installer");
    char *source = prepared != NULL ? net_file(prepared) : NULL, *a, *b, *both;
    int ok = 0;
    if (source == NULL)
        return 0;
    a = function(source, "add_lines");
    b = function(source, "merge_accounts");
    both = a != NULL && b != NULL ? append(a, b) : NULL;
    if (both != NULL) {
        char *program = append(both, "\nmerge_accounts \"$1\" \"$2\" \"$3\"\n");
        ok = program != NULL && image_text(t, "functions", program);
        free(program);
    }
    free(a);
    free(b);
    free(both);
    free(source);
    return ok;
}

int image_accounts(struct image_check *t)
{
    unsigned i;
    if (!source_functions(t))
        return 0;
    for (i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        const struct merge_row *r = &rows[i];
        int ok = account_files(t, "image", &r->image) && account_files(t, "dest", &r->before) &&
                 merge(t, r->scope, r->printed) && same_accounts(t, &r->after);
        image_row(t, r->row, ok);
    }
    return t->failed == 0;
}
