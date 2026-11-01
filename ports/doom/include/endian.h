/*
 * endian.h: the byte order, for Doom on Rach2, in place of musl's, which
 * <sys/soundcard.h> includes and whose uint32_t is not i686-elf-gcc's.
 */
#ifndef _ENDIAN_H
#define _ENDIAN_H

#define __LITTLE_ENDIAN 1234
#define __BIG_ENDIAN    4321
#define __BYTE_ORDER    __LITTLE_ENDIAN /* x86 */

#define LITTLE_ENDIAN __LITTLE_ENDIAN
#define BIG_ENDIAN    __BIG_ENDIAN
#define BYTE_ORDER    __BYTE_ORDER

#endif /* _ENDIAN_H */
