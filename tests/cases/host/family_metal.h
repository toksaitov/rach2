/* family_metal.h: deployment-product transport and password fixtures. */
#ifndef HOST_FAMILY_METAL_H
#define HOST_FAMILY_METAL_H
#include "family_check.h"
int family_metal_password(struct fixture *f);
int family_metal_transport(struct fixture *f);
int family_metal_mock(int argc, char **argv);
int family_metal_ssh(const char *command);
#endif
