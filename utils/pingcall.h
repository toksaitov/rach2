#ifndef PINGCALL_H
#define PINGCALL_H

/*
 * pingcall.h: Rach2's ICMP echo, which no libc has a wrapper for.
 *
 * The echo is a sub-call of socketcall(2), numbered clear of the BSD
 * sub-calls. SC_PING takes the destination; SC_PING_DATA takes the
 * destination, the data size and the hop limit. Each argument is an unsigned
 * long in the array whose address is socketcall's second argument, and the
 * kernel sends the echo and sleeps the caller until the reply, for up to four
 * attempts of a second each.
 *
 * i386 has socketcall as call 102. x86-64 has no socketcall: the kernel takes
 * Rach2's at 400, a number Linux x86-64 leaves unassigned below the 424 from
 * which every architecture's numbers agree, and serves the two sub-calls
 * there.
 */

#define SC_PING      0x1000
#define SC_PING_DATA 0x1001

#if defined __x86_64__
#define SYS_PINGCALL 400
#else
#define SYS_PINGCALL 102
#endif

#endif /* PINGCALL_H */
