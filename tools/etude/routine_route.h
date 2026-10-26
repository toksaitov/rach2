/* Fixed phase actions called by the existing native routine owners. */
#ifndef ETUDE_ROUTINE_ROUTE_H
#define ETUDE_ROUTINE_ROUTE_H
#include <signal.h>
int etude_prepare_route_main(int argc, char **argv, volatile sig_atomic_t *stop);
#endif
