Milestone 43, phase 7: the differences.
Evidence gathered: phases 1 to 6 closed; the acceptance script passes on
the machine and in QEMU; the machine runs the tree's kernel on the ext2
root with the hub, the keyboard, the mouse and the recovery stick on it.
Open differences, in notes.md under Phases 1 to 5 and Open: the frame
exhaustion under the lag; the stick under nomsi; the clock self-test;
the RTC's zone; root's key after the stick boot; the FAT32 write rate;
the large IWAD; the SuperSpeed hub; the stick on the hub at boot; the
recovery partition without e2fsck.
Next action: take the differences one at a time, each in its subsystem
with a row, its suites, and a machine rerun; the SuperSpeed hub last
since it is the largest.
File being read when written: none.
