/* login_scenario.c: console authentication and recovery, one boot per case. */
#include <stdlib.h>
#include <string.h>

#include "netcase.h"

/** Run the selected init policy, stopped session or rescue boot. */
int etude_scenario(struct etude_case *c)
{
    struct net_case u;
    struct net_boot b;
    struct etude_termination end;
    const char *id = strrchr(etude_case_id(c), '.') + 1;
    char password[128], *text, *reply = NULL;

    net_case(&u, c);
    net_step(&u, "boot");
    net_boot(&b, &u, "fixture", 1, NULL);
    if (strcmp(id, "strict") == 0) {
        net_wait(&b, "init: cannot exec /bin/login");
        net_line(&b, "id");
        net_wait(&b, "init: cannot exec /bin/login");
        net_assert(&u, "retries", 1, "init did not retry the missing login");
        net_seen(&b, "init: boot rach2.rescue");
        net_assert(&u, "advice", 1, "init gave no rescue instructions");
    } else if (strcmp(id, "shell") == 0) {
        net_wait(&b, "~ ");
        net_command(&b, "id; echo ID\"\"DONE", "IDDONE", &text);
        net_assert(&u, "root-shell", text != NULL && net_find(text, "uid=0") != NULL,
                   "the permissive image did not run id as root");
        free(text);
    } else if (strcmp(id, "stopped") == 0) {
        net_secret(&u, "password", password, sizeof(password));
        net_login(&b, "root", password, "# ");
        net_line(&b, "kill -STOP $$; echo CONTINUED\"\"-OK");
        net_wait(&b, "CONTINUED-OK");
        net_assert(&u, "continued", 1, "init did not continue the stopped session");
    } else {
        net_wait(&b, "rescue mode");
        net_wait(&b, NET_SHELL);
        net_assert(&u, "rescue", 1, "rescue did not reach the debug shell");
    }
    if (net_ok(&u)) {
        if (etude_qmp(b.m, "quit", NULL, &reply) != ETUDE_OK ||
            etude_wait_end(b.m, &end) != ETUDE_OK)
            net_fail(&u, "the boot did not end");
        free(reply);
    }
    if (strcmp(id, "strict") == 0) {
        text = net_file(b.console);
        net_assert(&u, "closed", strstr(text, "uid=0") == NULL,
                   "the strict image reached an unauthenticated root shell");
        free(text);
    }
    return net_end(&u);
}
