/*
 * pingcall.h: the numbers of Rach2's ICMP echo, a system call no libc has a
 * wrapper for, which the test probes make (probe.h).
 *
 * The echo is three sub-calls of socketcall(2), numbered clear of the BSD
 * sub-calls. Each argument is an unsigned long in the array whose address is
 * socketcall's second argument. The kernel sends the echo and the caller
 * sleeps until the reply, for up to four attempts of a second each; a signal
 * ends the wait with EINTR. Each call has an echo identifier of its own, so
 * calls run at once and each takes only its own reply.
 *
 * i386 has socketcall as call 102. x86-64 has no socketcall: the kernel takes
 * Rach2's at 400, a number Linux x86-64 leaves unassigned below the 424 from
 * which every architecture's numbers agree, and serves only the three
 * sub-calls there.
 */
#ifndef PINGCALL_H
#define PINGCALL_H

/* An echo to the destination, an IPv4 address as an integer whose high byte
 * is the first octet. Returns the round trip in kernel ticks. */
#define SC_PING      0x1000
/* An echo to the destination with the data size, at most 65507 bytes, sent in
 * fragments past 1472, and the hop limit, 1 to 255. Returns the reply's ICMP
 * length in the low 16 bits and the hop limit it arrived with in the 8 bits
 * above them. */
#define SC_PING_DATA 0x1001
/* An ICMPv6 echo: the address of the destination's 16 bytes in network
 * order, the index of the interface a link-local destination is reached by
 * or 0, the data size, at most 65527 bytes, sent in fragments past 1452,
 * and the hop limit, 1 to 255.
 * Returns as SC_PING_DATA does. */
#define SC_PING6     0x1002

#if defined __x86_64__
#define SYS_PINGCALL 400
#else
#define SYS_PINGCALL 102
#endif

#endif /* PINGCALL_H */
