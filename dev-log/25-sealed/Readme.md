# Milestone 25: the sealed disk

The plan is in `plan.md`, written before any code and put through four rounds of adversarial review before a line was implemented. This file records what was built, what was proved and where, what was refuted along the way, and what is still waiting for the hardware.

The goal is a laptop whose disk is written once and never removed again. Everything that changes afterwards arrives over SSH: kernels, the boot menu, GRUB itself, and in the worst case the whole disk from a recovery image.

## Phase A: the receiver window update

Milestone 24 measured bulk transfer at 0.9 KB/s on the laptop against 1.1 KB/s in QEMU, and reasoned from how close those two numbers are that the fault lay in the TCP stack rather than in the new driver or the 100 Hz poll. That reasoning was right. Its record then called the result "a property of the TCP stack", and that part was wrong: it is one defect, in one direction, with a one-function fix.

A TCP receiver has to report a window that shrank and a window that grew. This stack only reported the first. The inbound paths in `tcp.c` recompute `rcv_wnd` whenever a segment arrives, so a filling buffer was announced at once. A draining buffer was announced to nobody, because `socket_tcp_recv` and `socket_tcp_try_recv` advance `recv_head` in syscall context and sent nothing. A peer that had filled the window waited for its own persist timer before probing again, and that timer set the transfer rate.

The fix is `tcp_window_update`, called from both receive paths after `recv_head` advances. It takes `net_lock` itself and holds it from the TCB snapshot through the send, because an IRQ-side `tcp_close` detaches the TCB. It sends at most one bare ACK and only from ESTABLISHED, where the closing states have no reader making room. Whether to send is the whole of `rach2/sys/network/tcp/rcvwnd.c`: a pure function, no state and no transmit, that stays quiet until the window has grown by one maximum segment size or half the buffer, whichever is smaller. That is the RFC 1122 4.2.3.3 rule. Announcing sooner would spend a segment per byte read and draw segments just as small in reply.

Measured host to guest over SSH into `/dev/null`, same image, same QEMU invocation, with the call removed and restored:

| Bytes   | Without | With    | Rate without | Rate with |
|---------|---------|---------|--------------|-----------|
| 4096    |         | 0.361 s |              | 11.4 KB/s |
| 65536   | 60.179s | 0.291 s | 1089 B/s     | 225 KB/s  |
| 1048576 |         | 0.408 s |              | 2.57 MB/s |

The 64 KiB figure without the update reproduces milestone 24's measurement to three digits, which is what identifies the two as the same defect rather than two separate slow things.

Proof: `tests/host/rcvwnd_hosttest.c` drives the rule at ring and segment sizes the running kernel never uses, because which half of the rule wins depends on the geometry: at 4 KiB one segment is smaller than half the buffer, at 8 bytes half the buffer is smaller. Four mutations were shown to fail it: never announcing, announcing every byte, using two segments instead of one, and dropping the 16-bit clamp. The measurement above is the other half, and the column without the update is the phase's shown-failing case.

### What the review changed

An independent review of the phase raised five findings. Three were confirmed and fixed in the same commit. The bar was two maximum segment sizes, which is not the RFC 1122 rule; it is one segment or half the buffer, whichever is smaller, and the old value made the receiver quieter than it should be. The returned window was cast to 16 bits without a clamp, so a ring of 65536 would have announced zero, which this function uses to mean stay quiet, and a fully drained buffer would have announced nothing. And the new function was inserted between `tcp_is_connected`'s documentation block and `tcp_is_connected` itself, stranding the block on the wrong function.

### What is deferred, and why

The rule governs the update a reader triggers, not every window the stack advertises. The inbound paths still recompute `rcv_wnd` from free space whenever they owe an ACK, so a segment arriving after a small read can advertise a window below the bar. This predates the milestone, and the update path largely hides it: a reader draining normally announces a full-sized window before a peer has cause to probe. Correcting it means holding the advertised right edge fixed as data is accepted rather than recomputing it from free space, which changes the receive state machine that phase A deliberately does not touch, and it needs a stateful test to be proved rather than asserted. It is the next thing worth chasing in the stack.

The host test covers the rule, not the wiring: deleting both call sites leaves it passing. The wiring is proved by measurement instead, run once with both call sites present and once with them removed, and that measurement is manual. Every figure here is host to guest; the reverse direction was never slow and was not measured again. An automated guard would need a packet recorder around a live connection, which is worth building when something else needs one too.
