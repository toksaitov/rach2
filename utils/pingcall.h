/*
 * pingcall.h: the numbers of Rach2's ICMP echo, a system call no libc has a
 * wrapper for, shared by ping.c and the test probes.
 *
 * The echo is two sub-calls of socketcall(2), numbered clear of the BSD
 * sub-calls. Each argument is an unsigned long in the array whose address is
 * socketcall's second argument. The kernel sends the echo and the caller
 * sleeps until the reply, for up to four attempts of a second each; a signal
 * ends the wait with EINTR, and a second caller while one echo is in flight
 * gets EBUSY.
 *
 * i386 has socketcall as call 102. x86-64 has no socketcall: the kernel takes
 * Rach2's at 400, a number Linux x86-64 leaves unassigned below the 424 from
 * which every architecture's numbers agree, and serves only the two sub-calls
 * there.
 */
#ifndef PINGCALL_H
#define PINGCALL_H

/* An echo to the destination, an IPv4 address as an integer whose high byte
 * is the first octet. Returns the round trip in kernel ticks. */
#define SC_PING      0x1000
/* An echo to the destination with the data size, at most 1472 bytes, and the
 * hop limit, 1 to 255. Returns the reply's ICMP length in the low 16 bits and
 * the hop limit it arrived with in the 8 bits above them. */
#define SC_PING_DATA 0x1001

#if defined __x86_64__
#define SYS_PINGCALL 400
#else
#define SYS_PINGCALL 102
#endif

#endif /* PINGCALL_H */
