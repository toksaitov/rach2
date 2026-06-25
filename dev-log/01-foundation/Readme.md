# Milestone 1: the foundation OS

This milestone grew Rach2 from a bare-metal protected-mode kernel into a bootable 32-bit OS that runs Doom: physical and virtual memory with paging and a heap, kernel and user processes with syscalls and ELF loading, the KRFFS RAM disk file system, the device drivers (VGA Mode 13h, the PS/2 keyboard and mouse, the PIT timer, the PC speaker, and Sound Blaster 16 and Intel HDA audio with OPL3 music, including real-hardware HDA bring-up for Doom), the ish shell with Ctrl-C process control, and the Doom port through doomgeneric and Chocolate Doom.

It was driven by 35 development prompts, recorded here as `01.md` through `35.md`, with `Plan.md` as the plan and progress log. Milestone 2 grew it into a networked system running a musl userland.
