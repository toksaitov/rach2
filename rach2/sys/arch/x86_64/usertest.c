/*
 * usertest.c: the debug shell's ring-3 exercise on x86-64, a payload the
 * shell copies into a user page and runs as a process of its own.
 */

#include "../arch.h"

/*
 * The payload, position-independent, with its message after it:
 *
 *     mov  $1, %eax          write(1, msg, 19), Linux x86-64's call 1
 *     mov  $1, %edi
 *     lea  msg(%rip), %rsi   18 bytes past the end of this instruction
 *     mov  $19, %edx
 *     syscall
 *     mov  $60, %eax         exit(0)
 *     xor  %edi, %edi
 *     syscall
 *     jmp  .
 * msg: "Hello from ring 3!\n"
 *
 * Bytes, since the payload is data the shell copies.
 */
const unsigned char arch_user_test_code[] = {
    0xB8, 0x01, 0x00, 0x00, 0x00, 0xBF, 0x01, 0x00, 0x00, 0x00, 0x48, 0x8D, 0x35, 0x12,
    0x00, 0x00, 0x00, 0xBA, 0x13, 0x00, 0x00, 0x00, 0x0F, 0x05, 0xB8, 0x3C, 0x00, 0x00,
    0x00, 0x31, 0xFF, 0x0F, 0x05, 0xEB, 0xFE, 'H',  'e',  'l',  'l',  'o',  ' ',  'f',
    'r',  'o',  'm',  ' ',  'r',  'i',  'n',  'g',  ' ',  '3',  '!',  '\n'};

const unsigned int arch_user_test_size = sizeof(arch_user_test_code);
