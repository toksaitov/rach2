Milestone 43, the machine: closed on 2026-09-04. The procedures for
working the machine from the host are in Readme.md under "Updating the
installed machine" and "Working the machine from the host"; notes.md
has every difference with its state, hardware.md the readings, and
Readme.md in this directory what was proved and what remains open.

Phases 1 to 9 are closed. The machine runs its own build of the final
tree, `77b78018...` through TinyCC, on the ext2 root `9c20cabd...`,
with the KRFFS root as the fallback and a recovery partition that has
`e2fsck`; `bootctl kernel build/rach2/kernel` puts the host's build
back. The hub with the keyboard, the mouse and the recovery stick is
in the blue socket, and the machine plays the tree's own shareware
WAD as `/doom1.wad`, with Freedoom beside it as `/freedoom1.wad` for
the music test. The
acceptance script passes against it, and the stick beside it is the
recovery stick with an empty save volume. The full set, the documents'
review and every difference's row are done and recorded in notes.md.

Open, and each recorded there: the panic at a pull of a stick that had
stopped answering, which did not recur and whose record the netconsole
capture on the Mac would keep; and the frame exhaustion under the lag
device, seen once in phase 1 and not reproduced in two trials with the
record in place. Both wait on the machine showing them again. The
milestone's own Readme has the trust statement.
File being read when written: none.
