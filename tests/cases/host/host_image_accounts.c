/* host_image_accounts.c: run installer account functions on fixtures. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "host_image.h"

struct accounts {
    const char *passwd, *group, *shadow;
};
static const struct accounts image = {
    "root:x:0:0:root:/root:/bin/sh\ntest:x:1000:1000:test:/home/test:/bin/sh\n"
    "sshd:x:74:74::/var/empty:/usr/bin/false\ndemo:x:1002:1002:demo:/home/demo:/bin/sh\n",
    "root:x:0:\ntest:x:1000:\nsshd:x:74:\n",
    "root:*:0:0:99999:7:::\ntest:*:0:0:99999:7:::\nsshd:!:0:0:99999:7:::\n"};
static const struct accounts machine = {
    "root:x:0:0:root:/root:/bin/sh\nowner:x:1001:1001:owner:/home/owner:/bin/sh\n",
    "root:x:0:\nowner:x:1001:\n", "root:$6$a:0:0:99999:7:::\nowner:$6$b:0:0:99999:7:::\n"};
static const struct accounts sshd = {"sshd:x:74:74::/var/empty:/usr/bin/false\n", "sshd:x:74:\n",
                                     "sshd:!:0:0:99999:7:::\n"};

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

/** Run the product operation; the controller judges its files. */
static int keep(struct image_check *t)
{
    char functions[1024], image_path[1024], dest[1024];
    const char *argv[] = {"/bin/sh", functions, image_path, dest, NULL};
    snprintf(functions, sizeof(functions), "%s/functions", t->dir);
    snprintf(image_path, sizeof(image_path), "%s/image", t->dir);
    snprintf(dest, sizeof(dest), "%s/dest", t->dir);
    return image_expect(t, argv, "");
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

static void fixture(struct image_check *t, const char *row, const struct accounts *from,
                    const struct accounts *before, const struct accounts *after)
{
    int ok = account_files(t, "image", from) && account_files(t, "dest", before) && keep(t) &&
             same_accounts(t, after);
    image_row(t, row, ok);
}

static int source_functions(struct image_check *t)
{
    const char *prepared = etude_artifact(t->u.c, "host-source-installer");
    char *source = prepared != NULL ? net_file(prepared) : NULL, *a, *b, *both;
    int ok = 0;
    if (source == NULL)
        return 0;
    a = function(source, "add_lines");
    b = function(source, "keep_image_accounts");
    both = a != NULL && b != NULL ? append(a, b) : NULL;
    if (both != NULL) {
        char *program = append(both, "\nkeep_image_accounts \"$1\" \"$2\"\n");
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
    struct accounts expected, changed, from;
    char *joined[8];
    unsigned i;
    if (!source_functions(t))
        return 0;
    joined[0] = append(machine.passwd, sshd.passwd);
    joined[1] = append(machine.group, sshd.group);
    joined[2] = append(machine.shadow, sshd.shadow);
    joined[3] = append(machine.passwd, "other:x:074:74::/:/bin/sh\n");
    joined[4] = append(machine.passwd, "sshd:x:80:80::/:/bin/sh\n");
    joined[5] = append(image.passwd, "sshd:x:74:74::/var/empty:/usr/bin/false\n"
                                     "sshd2:x:74:74::/:/bin/sh\n");
    joined[6] = append(image.group, sshd.group);
    joined[7] = append(image.shadow, sshd.shadow);
    for (i = 0; i < 8; i++)
        if (joined[i] == NULL) {
            t->failed++;
            goto end;
        }
    expected = (struct accounts){joined[0], joined[1], joined[2]};
    fixture(t, "append only missing system accounts", &image, &machine, &expected);
    image_row(t, "a second run adds nothing", keep(t) && same_accounts(t, &expected));
    changed = machine;
    changed.passwd = joined[3];
    fixture(t, "numeric ID conflict including leading zero", &image, &changed, &changed);
    changed.passwd = joined[4];
    fixture(t, "account name conflict", &image, &changed, &changed);
    from = (struct accounts){NULL, NULL, NULL};
    fixture(t, "no image account files", &from, &machine, &machine);
    from = (struct accounts){joined[5], joined[6], joined[7]};
    fixture(t, "duplicate names and IDs are added once", &from, &machine, &expected);
    changed = machine;
    changed.passwd = "root:x:0:0:root:/root:/bin/sh\nowner:x:1001:1001:owner:/home/owner:/bin/sh";
    fixture(t, "keep a final line without newline whole", &image, &changed, &expected);
    changed = machine;
    changed.group = "";
    from = expected;
    from.group = "root:x:0:\nsshd:x:74:\n";
    fixture(t, "empty destination group gets system groups", &image, &changed, &from);
    changed = image;
    changed.shadow = "root:*:0:0:99999:7:::\n";
    from = expected;
    from.shadow = machine.shadow;
    fixture(t, "missing image shadow line leaves destination shadow", &changed, &machine, &from);
    changed = image;
    changed.group = append(image.group, "crontab:x:63:\n");
    from = expected;
    from.group = append(expected.group, "crontab:x:63:\n");
    if (changed.group != NULL && from.group != NULL)
        fixture(t, "system group without a new account", &changed, &expected, &from);
    else
        t->failed++;
    free((void *)changed.group);
    free((void *)from.group);
    changed = image;
    changed.passwd = append(image.passwd, "nobody:x:65534:65534::/:/bin/false\n");
    changed.shadow = append(image.shadow, "nobody:!:::::::\n");
    from = expected;
    from.passwd = append(expected.passwd, "nobody:x:65534:65534::/:/bin/false\n");
    from.shadow = machine.shadow;
    expected.passwd = from.passwd;
    expected.shadow = append(joined[2], "nobody:!:::::::\n");
    if (changed.passwd && changed.shadow && from.passwd && expected.shadow)
        fixture(t, "existing system accounts gain missing image shadow lines", &changed, &from,
                &expected);
    else
        t->failed++;
    free((void *)changed.passwd);
    free((void *)changed.shadow);
    free((void *)from.passwd);
    free((void *)expected.shadow);
end:
    for (i = 0; i < 8; i++)
        free(joined[i]);
    return t->failed == 0;
}
