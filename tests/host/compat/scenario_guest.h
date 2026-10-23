/* The native Windows actor behind the scenario's stand-in serial console. */
#ifndef RACH2_SCENARIO_GUEST_H
#define RACH2_SCENARIO_GUEST_H
#include "desktop_host.h"
#ifdef _WIN32
long scenario_guest_start(const char *self, host_socket console);
int scenario_guest_actor(const char *port);
int scenario_relay(const char *port);
#endif
#endif
