/*
 * hosts.h: the host system etude runs on, and whether a case's
 * `requirements.hosts` lets the case run on a host system.
 */
#ifndef ETUDE_HOSTS_H
#define ETUDE_HOSTS_H

#include "json.h"

const char *etude_host_system(void);
int etude_case_hosted(const struct json *c, const char *host);
int etude_case_runs_here(const struct json *c);

#endif
