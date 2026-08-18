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

## Phase B: the tool's new name and shape

`install-kernel` is now `bootctl`, after systemd's tool, which manages a boot partition's kernels and defaults on most Linux machines; a student who learns this one carries the knowledge off the machine. The verbs stay this project's own, because the kernel diagnostics, the Readmes and the harness already speak trial and commit:

    bootctl status         what is installed, committed, and booting next
    bootctl kernel FILE    stage a kernel into the candidate slot
    bootctl commit         make the booted trial the saved default

No alias is shipped. `--commit`, `--where` and a bare kernel path are refused, and there is a test that says so, because a compatibility spelling nobody documents has to be carried forever to be worth anything.

Three boundaries held the change to twenty-one files. The `bootslot` library keeps its name, since it is named for what it manages rather than for the tool that drives it, and two dozen of its error substrings are matched literally by a host test. `dev-log/` keeps its history as written. `make install-test` keeps its name, because the loop it proves still installs a kernel.

`status` is the phase's one new behavior, and it exists because the tool is the only window onto a disk nobody will open: the three files' sector extents, what the environment block holds, whether a trial is armed or consumed, and which slot this boot came from. It says nothing about the GRUB core image, because the device that would answer that does not exist until phase C.

Two changes were made that the phase did not strictly need, both to close hazards the plan predicted.

`prune-stale` now sweeps `build/utils/sbin` as well as the top of `build/utils`. It did not before, on the reasoning that sbin's contents are staged by name rather than by sweep. That reasoning has a hole: a manifest line left on the old name would keep staging the old binary, and `check_setid.py` compares a manifest's declared source against its own table without ever stat-ing the host file, so nothing would report it. The rename left exactly that artifact behind on the first build, which is what closed the gap.

The harness's `awk` was anchored. `tools/install_test.sh` read the candidate's first sector with `/candidate/{print $5}`, which was unambiguous when `--where` printed three lines and is not now that `status` prints six, three of which contain the word. In the shipped line order the loose pattern still happens to work, because the extra matches yield an empty field five and the shell strips the trailing newlines; put a non-extent line first and it yields a leading newline instead, and `dd` gets a malformed seek. The pattern is now anchored on the extent line's own shape, which does not depend on the order.

Proof: `make install-test` green end to end, twice through the loop on one disk, which is what shows the trial, commit, fallback and repair machinery survived the rename; `make selfbuild-test` green, which builds `bootctl` from `guest.mk` on the machine itself and so proves the guest list moved with the host list. `tests/host/bootverb_hosttest.c` covers the parser.

Shown failing: the harness pointed at the old `install-kernel: committed` string fails the run, so that assertion is reaching the tool's output rather than passing vacuously. Two mutations of the parser fail its host test: accepting `--commit` again, and tolerating a surplus operand. The anchored `awk` against a reworded extent line yields nothing rather than a wrong sector.

### What the review changed

Six findings, all confirmed. Two were defects in new code and two were in code the rename only touched in passing.

`status` called every empty environment value a consumed trial. Only `next_entry` carries that meaning; an empty `saved_entry` says nothing about a trial. The annotation is now asked for by the caller rather than assumed by the printer.

A known verb given the wrong number of operands was reported as though it were not a verb at all, so `bootctl kernel` with no path answered "not a command this tool takes: 'kernel'". Exit status was right and the message was not. `bootverb_known` now separates the two, and arity refusals print the usage line alone, as they did before the rename.

`bootslot_commit` matched the armed-trial marker as a bare 21-byte string with no leading newline, so a preserved line ending in `next_entry=candidate` would have been read as this machine's armed trial and blocked a commit that should have succeeded. The writer deliberately preserves lines it does not recognise, so such a line can exist. The pattern is now anchored the way the consumed marker already was, both bounds are checked independently so neither shortens the other's search, and a fixture carrying `x_next_entry=candidate` beside a real consumed marker proves it. This is older than the milestone; the rename found it by putting a second reader on the same block.

The parser test asserted only that `kernel` yielded some operand, not which one. A parser returning `argv[1]` instead of `argv[2]` passed it, and would have made `bootctl kernel FILE` try to open a file called `kernel`. The test now checks the value, and that mutation fails it.

The remaining two were smaller. `utils/Readme.md` still described `--commit`, named the host test by its old filename, and said pruning covered only the top level, which stopped being true in this phase. And the new parser test carried section labels inside its body, which the project's comment rule does not allow; they are gone.

A second round over those fixes found one more and refuted nothing. `bootverb_parse` and `bootverb_known` had grown separate copies of the verb list, so a verb added to one and missed in the other would compile and then report itself as no verb at all. They share one catalog now, and a test walks it: every verb the parser accepts must also be one the diagnostic knows, which fails if the two ever drift.

One finding from that round was refuted with evidence. The new fixture in `tests/host/bootctl_hosttest.c` opens with a comment introducing the case, which the comment rule reads against. That file is a sequence of eighteen such cases, each introduced the same way, and the rule is written for production bodies rather than for a test whose structure is its scenarios. Following the file is right here; restyling it is a separate cleanup, not part of a rename.
