/* Native preparation and compatibility image commands in Etude. */
#ifndef ETUDE_COLLECTION_TOOLS_H
#define ETUDE_COLLECTION_TOOLS_H
int etude_collection_native_goal(const char *goal);
/* ARGV[0] is list, tool, prepare or style. */
int etude_collection_tools_main(int argc, char **argv);
#endif
