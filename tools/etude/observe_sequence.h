/* Native ownership of preparation and collection under one observation. */
#ifndef ETUDE_OBSERVE_SEQUENCE_H
#define ETUDE_OBSERVE_SEQUENCE_H

/** Run the counted preparation and collection argv vectors. */
int etude_observe_sequence_main(int argc, char **argv);
int etude_observe_parent_main(int argc, char **argv);
struct etude_pid;
int etude_sequence_parent_identity(long parent, struct etude_pid *identity);
int etude_sequence_caller_identity(struct etude_pid *identity);
#ifndef _WIN32
int etude_posix_observe_sequence_main(int argc, char **argv);
int etude_posix_observe_parent_main(int argc, char **argv);
#endif

#endif
