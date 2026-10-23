/* Native ownership of preparation and collection under one observation. */
#ifndef ETUDE_OBSERVE_SEQUENCE_H
#define ETUDE_OBSERVE_SEQUENCE_H

/** Run the counted preparation and collection argv vectors. */
int etude_observe_sequence_main(int argc, char **argv);
#ifndef _WIN32
int etude_posix_observe_sequence_main(int argc, char **argv);
#endif

#endif
