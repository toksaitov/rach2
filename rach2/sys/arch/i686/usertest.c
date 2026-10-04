/*
 * usertest.c: the debug shell's ring-3 exercise on an i686, a payload the
 * shell copies into a user page and runs as a process of its own.
 */

#include "../arch.h"

/*
 * The payload, position-independent, with its message after it:
 *
 *     mov  $4, %eax          write(1, msg, 19), through int 0x80
 *     mov  $1, %ebx
 *     call 1f
 * 1:  pop  %ecx              the address of this byte
 *     add  $22, %ecx         msg, 22 bytes on
 *     mov  $19, %edx
 *     int  $0x80
 *     mov  $1, %eax          exit(0)
 *     xor  %ebx, %ebx
 *     int  $0x80
 *     jmp  .
 * msg: "Hello from ring 3!\n"
 *
 * Bytes, since the payload is data the shell copies: the staged TinyCC's
 * assembler and GNU as both accept a C array.
 */
const unsigned char arch_user_test_code[] = {
    0xB8, 0x04, 0x00, 0x00, 0x00, 0xBB, 0x01, 0x00, 0x00, 0x00, 0xE8, 0x00, 0x00, 0x00,
    0x00, 0x59, 0x83, 0xC1, 0x16, 0xBA, 0x13, 0x00, 0x00, 0x00, 0xCD, 0x80, 0xB8, 0x01,
    0x00, 0x00, 0x00, 0x31, 0xDB, 0xCD, 0x80, 0xEB, 0xFE, 'H',  'e',  'l',  'l',  'o',
    ' ',  'f',  'r',  'o',  'm',  ' ',  'r',  'i',  'n',  'g',  ' ',  '3',  '!',  '\n'};

const unsigned int arch_user_test_size = sizeof(arch_user_test_code);
