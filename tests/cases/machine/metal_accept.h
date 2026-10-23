/* metal_accept.h: the C controller of the metal machine's acceptance. */
#ifndef METAL_ACCEPT_H
#define METAL_ACCEPT_H
#include "netcase.h"
struct metal_accept_request {
    struct net_login login;
    int use_su;
    const char *serial;
    const char *fat_image;
};
/* Run the machine checks of the native metal_accept.sh route in C. */
enum etude_status metal_accept_run(struct net_case *u, const struct metal_accept_request *request,
                                   const char *output, struct net_run *r);
#endif
