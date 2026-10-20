/*
 * macho.h: the digest of a 64-bit Mach-O file without the parts that its
 * linker derives from the paths and times of the objects it read.
 */
#ifndef ETUDE_MACHO_H
#define ETUDE_MACHO_H

int macho_digest(const char *path, char hex[65]);

#endif
