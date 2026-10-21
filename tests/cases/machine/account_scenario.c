/*
 * account_scenario.c: console account tools, authentication and durable
 * deletion.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "netcase.h"

/* One line typed or one answer awaited at the console. */
struct operation {
    int send;
    const char *text;
};

static const struct operation simple[] = {{1, "pwd"},
                                          {0, "/root"},
                                          {1, "adduser newbie"},
                                          {0, "created with uid"},
                                          {0, "New password:"},
                                          {1, "newbiepw1"},
                                          {0, "Retype new password:"},
                                          {1, "newbiepw1"},
                                          {0, "password changed for newbie"},
                                          {1, "cat /etc/passwd"},
                                          {0, "newbie:x:"},
                                          {1, "exit"},
                                          {0, "rach2 login:"},
                                          {1, "test"},
                                          {0, "Password:"},
                                          {1, "@test-password@"},
                                          {1, "pwd"},
                                          {0, "/home/test"},
                                          {1, "echo TRYWRONG"},
                                          {0, "TRYWRONG"},
                                          {1, "su - newbie -c pwd"},
                                          {0, "Password:"},
                                          {1, "notthepassword"},
                                          {0, "authentication failure"},
                                          {1, "echo TRYRIGHT"},
                                          {0, "TRYRIGHT"},
                                          {1, "su - newbie -c pwd"},
                                          {0, "Password:"},
                                          {1, "newbiepw1"},
                                          {0, "/home/newbie"},
                                          {1, "echo TRYDONE"},
                                          {0, "TRYDONE"},
                                          {1, "passwd"},
                                          {0, "Old password:"},
                                          {1, "@test-password@"},
                                          {0, "New password:"},
                                          {1, "testeepw2"},
                                          {0, "Retype new password:"},
                                          {1, "testeepw2"},
                                          {0, "password changed"},
                                          {1, "echo TRYOLD"},
                                          {0, "TRYOLD"},
                                          {1, "su - test -c pwd"},
                                          {0, "Password:"},
                                          {1, "@test-password@"},
                                          {0, "authentication failure"},
                                          {1, "echo TRYOTHER"},
                                          {0, "TRYOTHER"},
                                          {1, "su - newbie -c pwd"},
                                          {0, "Password:"},
                                          {1, "testeepw2"},
                                          {0, "authentication failure"},
                                          {1, "echo TRYNEW"},
                                          {0, "TRYNEW"},
                                          {1, "su - test -c pwd"},
                                          {0, "Password:"},
                                          {1, "testeepw2"},
                                          {0, "/home/test"},
                                          {1, "echo TRYEND"},
                                          {0, "TRYEND"},
                                          {0, NULL}};

static const struct operation create[] = {
    {1, "adduser -D testee && addgroup shared && addgroup testee shared && echo MADE\"\"IT"},
    {0, "MADEIT"},
    {1, "adduser -S -D -H -G shared sysacct && echo SYS\"\"IT"},
    {0, "SYSIT"},
    {1, "echo testee:testeepw1 | chpasswd && echo SET\"\"IT"},
    {0, "SETIT"},
    {1, "su testee -c \"su testee -c id && echo AUTH\"\"IT\""},
    {0, "Password:"},
    {1, "testeepw1"},
    {0, "AUTHIT"},
    {1, "su testee -c \"su root -c id && echo CLIM\"\"BED; echo TRIED\"\"IT\""},
    {0, "Password:"},
    {1, "notthepassword"},
    {0, "TRIEDIT"},
    {1, "su testee -c \"passwd; echo REFU\"\"SED\""},
    {0, "Old password:"},
    {1, "notthepassword"},
    {0, "REFUSED"},
    {1, "su testee -c \"passwd root 2>&1 | grep -q change && echo NOT\"\"MINE\""},
    {0, "NOTMINE"},
    {1, "su testee -c \"passwd && echo CHANGED\"\"IT\""},
    {0, "Old password:"},
    {1, "testeepw1"},
    {0, "New password:"},
    {1, "testeepw2"},
    {0, "Retype password:"},
    {1, "testeepw2"},
    {0, "CHANGEDIT"},
    {1, "sync; echo SYNC\"\"IT"},
    {0, "SYNCIT"},
    {0, NULL}};

static const struct operation change[] = {
    {1, "su testee -c \"su testee -c id && echo AGAIN\"\"IT\""},
    {0, "Password:"},
    {1, "testeepw2"},
    {0, "AGAINIT"},
    {1, "mkdir -p /www; echo SERVED\"\"OK > /www/index.html; echo MADE\"\"WWW"},
    {0, "MADEWWW"},
    {1, "httpd -m hunter2 > /tmp/webhash; cat /tmp/webhash; echo HASH\"\"ED"},
    {0, "HASHED"},
    {1, "echo /:webuser:$(cat /tmp/webhash) > /etc/httpd.conf; httpd -p 8080 -h /www; echo "
        "SERV\"\"ING"},
    {0, "SERVING"},
    {1, "echo ANON\"\"BEGIN; wget -q -O - http://127.0.0.1:8080/index.html 2>&1; echo ANON\"\"END"},
    {0, "ANONEND"},
    {1, "echo AUTH\"\"BEGIN; wget -q -O - http://webuser:hunter2@127.0.0.1:8080/index.html 2>&1; "
        "echo AUTH\"\"END"},
    {0, "AUTHEND"},
    {1, "mkpasswd -m sha512 -S abcdefgh hunter2; echo MK\"\"END"},
    {0, "MKEND"},
    {1, "echo BEG\"\"IN; cat /etc/passwd; echo PW\"\"END"},
    {0, "PWEND"},
    {1, "cat /etc/shadow; echo SH\"\"END"},
    {0, "SHEND"},
    {1, "cat /etc/group; echo GR\"\"END"},
    {0, "GREND"},
    {1, "deluser testee && deluser sysacct && sync && echo GONE\"\"IT"},
    {0, "GONEIT"},
    {0, NULL}};

static const struct operation deleted[] = {{1, "echo BEG\"\"IN; cat /etc/passwd; echo PW\"\"END"},
                                           {0, "PWEND"},
                                           {1, "cat /etc/shadow; echo SH\"\"END"},
                                           {0, "SHEND"},
                                           {1, "cat /etc/group; echo GR\"\"END"},
                                           {0, "GREND"},
                                           {0, NULL}};

/** Run the console operations, substituting the staged test password. */
static void operations(struct net_boot *b, const struct operation *op, const char *password)
{
    for (; op->text != NULL && net_ok(b->u); op++) {
        const char *text = strcmp(op->text, "@test-password@") == 0 ? password : op->text;

        if (op->send)
            net_line(b, text);
        else
            net_wait(b, text);
    }
}

/** End a boot by QMP, then give its complete console from malloc. */
static char *finish(struct net_boot *b)
{
    struct etude_termination end;
    char *reply = NULL;

    if (net_ok(b->u)) {
        if (etude_qmp(b->m, "quit", NULL, &reply) != ETUDE_OK ||
            etude_wait_end(b->m, &end) != ETUDE_OK)
            net_fail(b->u, "the account boot did not end");
        free(reply);
    }
    return net_file(b->console);
}

/** Say whether text between markers a and z has yes and lacks no. */
static int between(const char *text, const char *a, const char *z, const char *yes, const char *no)
{
    const char *start = strstr(text, a), *stop, *p;

    if (start == NULL || (stop = strstr(start + strlen(a), z)) == NULL)
        return 0;
    p = strstr(start, yes);
    if (p == NULL || p >= stop)
        return 0;
    p = no == NULL ? NULL : strstr(start, no);
    return p == NULL || p >= stop;
}

/** Report assertion id from a console pattern. */
static void has(struct net_case *u, const char *id, const char *text, const char *pattern)
{
    net_assert(u, id, net_find(text, pattern) != NULL, "missing: %s", pattern);
}

/** Say whether a passwd record gives an id in lo..hi. */
static int uid_range(const char *text, const char *name, long lo, long hi)
{
    const char *p = net_find(text, name);
    long n;

    if (p == NULL || (p = strchr(p, ':')) == NULL || (p = strchr(p + 1, ':')) == NULL)
        return 0;
    n = strtol(p + 1, NULL, 10);
    return n >= lo && n <= hi;
}

/** Exercise the in-house tools and each authentication result separately. */
static void simple_case(struct net_case *u, struct net_boot *b, const char *password)
{
    char *text;

    operations(b, simple, password);
    text = finish(b);
    has(u, "path", text, "/root");
    has(u, "created", text, "newbie:x:#:#:*:/home/newbie:/bin/sh");
    has(u, "test-home", text, "/home/test");
    net_assert(u, "wrong-refused",
               between(text, "TRYWRONG", "TRYRIGHT", "authentication failure", "/home/newbie"),
               "wrong password ran su");
    net_assert(u, "right-accepted",
               between(text, "TRYRIGHT", "TRYDONE", "/home/newbie", "authentication failure"),
               "correct password failed");
    has(u, "passwd", text, "password changed for test");
    net_assert(u, "old-refused",
               between(text, "TRYOLD", "TRYOTHER", "authentication failure", "/home/test"),
               "old password still worked");
    net_assert(u, "other-refused",
               between(text, "TRYOTHER", "TRYNEW", "authentication failure", "/home/newbie"),
               "password changed another account");
    net_assert(u, "new-accepted",
               between(text, "TRYNEW", "TRYEND", "/home/test", "authentication failure"),
               "new password failed");
    free(text);
}

/** Exercise BusyBox's tools over three boots of one private disk. */
static void real_case(struct net_case *u, struct net_boot *b, const char *password)
{
    char *text;
    unsigned i;
    static const char *const names[] = {"testee", "sysacct"};
    static const char hash[] =
        "$6$abcdefgh$M/"
        "eYsB4rVXAm3ZNc88J.UD9rCKAT6FB1rahiwJCHtEndQNORCub5qhjxn50qbqVVthkM.9HpEwtf0t.iV9uH0/";

    operations(b, create, password);
    text = finish(b);
    has(u, "chpasswd", text, "password for 'testee' changed");
    has(u, "authenticated", text, "uid=#(testee)");
    net_assert(u, "root-refused",
               strstr(text, "incorrect password") != NULL && strstr(text, "CLIMBED") == NULL &&
                   strstr(text, "uid=0(root)") == NULL,
               "wrong password reached root");
    has(u, "wrong-old-refused", text, "REFUSED");
    has(u, "other-refused", text, "NOTMINE");
    has(u, "passwd", text, "password for testee changed by testee");
    free(text);

    net_step(u, "read");
    net_boot(b, u, "fixture", 2, NULL);
    net_login(b, "root", password, "# ");
    operations(b, change, password);
    text = finish(b);
    has(u, "cold-authentication", text, "AGAINIT");
    has(u, "md5-hash", text, "$1$*$*");
    net_assert(u, "http-refused",
               between(text, "ANONBEGIN", "ANONEND", "401 Unauthorized", "SERVEDOK"),
               "anonymous HTTP served the page");
    net_assert(u, "http-authenticated", between(text, "AUTHBEGIN", "AUTHEND", "SERVEDOK", NULL),
               "httpd refused its own hash");
    has(u, "sha512-hash", text, hash);
    net_assert(u, "regular-id", uid_range(text, "testee:x:", 1000, 60000),
               "regular uid is outside 1000..60000");
    net_assert(u, "system-id", uid_range(text, "sysacct:x:", 100, 999),
               "system uid is outside 100..999");
    has(u, "shadow", text, "testee:$6$*");
    has(u, "membership", text, "shared:*:testee");
    free(text);

    net_step(u, "deleted");
    net_boot(b, u, "fixture", 3, NULL);
    net_login(b, "root", password, "# ");
    operations(b, deleted, password);
    text = finish(b);
    for (i = 0; i < 2; i++) {
        char record[40], member[40];

        snprintf(record, sizeof(record), "%s:", names[i]);
        snprintf(member, sizeof(member), ",%s", names[i]);
        net_assert(u, i == 0 ? "regular-deleted" : "system-deleted",
                   strstr(text, record) == NULL && strstr(text, member) == NULL &&
                       strstr(text, i == 0 ? ":testee" : ":sysacct") == NULL,
                   "deleted account remains in a database or member list");
    }
    has(u, "shared-survives", text, "shared:");
    free(text);
}

/** Run the selected image's account arc on its own boot media. */
int etude_scenario(struct etude_case *c)
{
    struct net_case u;
    struct net_boot b;
    char password[128], test_password[128];

    net_case(&u, c);
    net_secret(&u, "password", password, sizeof(password));
    net_secret(&u, "test-password", test_password, sizeof(test_password));
    net_step(&u, "create");
    net_boot(&b, &u, "fixture", 1, NULL);
    if (strstr(etude_case_id(c), ".simple") != NULL) {
        net_wait(&b, "login:");
        net_line(&b, "root");
        net_wait(&b, "Password:");
        net_line(&b, password);
        simple_case(&u, &b, test_password);
    } else {
        net_login(&b, "root", password, "# ");
        real_case(&u, &b, password);
    }
    return net_end(&u);
}
