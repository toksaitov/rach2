/* One reserved load, owned by the collection's native executor. */
#ifndef ETUDE_COLLECTION_LOAD_H
#define ETUDE_COLLECTION_LOAD_H
#include "etude/exec.h"
#include "etude/repeat.h"
struct etude_ledger;
struct etude_collection_load;
struct etude_collection_load_spec {
    const char *program, *claim, *workspace, *output;
    const struct etude_load *load;
    struct etude_ledger *ledger;
};
struct etude_collection_load *
etude_collection_load_start(const struct etude_collection_load_spec *);
int etude_collection_load_poll(struct etude_collection_load *);
int etude_collection_load_wait(struct etude_collection_load *, int milliseconds);
enum etude_cleanup etude_collection_load_stop(struct etude_collection_load *);
int etude_collection_load_failed(const struct etude_collection_load *);
struct etude_measure etude_collection_load_cpu(const struct etude_collection_load *);
const char *etude_collection_load_output(const struct etude_collection_load *, int error);
const char *etude_collection_load_record(struct etude_collection_load *);
void etude_collection_load_free(struct etude_collection_load *);
int etude_collection_load_register_case(void *, const char *, const struct etude_pid *,
                                        const struct etude_pid *);
int etude_collection_load_resources(const struct etude_load *, uint64_t resources[3]);
#endif
