# doom(1)

NAME: doom - Doom, on the framebuffer. Source: ports/doom/Readme.md.
* `/bin/doom`, a static program of the doomgeneric engine with a Rach2 platform layer: VGA mode 13h through the framebuffer device opened as `fb` relative to the current directory (`/fb` from `/`), 320 by 200 indexed frames and a 256-entry palette through `FB_SETPALETTE`; keyboard input as PS/2 set 1 scan codes on standard input (letters, digits, arrows, modifiers, function keys, Enter, Escape, Tab, the Doom action keys; at most 31 unread transitions); the mouse from `mouse` when it opens (three-byte packets), else keyboard only; time from gettimeofday(2), sleep a busy wait; PCM sound, PC speaker and synthesized music (Nuked OPL3) as the port's audio sources provide. It needs a WAD: `/demo/doom1.wad` is staged when the shareware WAD is present at the build (`optional-file`). It does not link musl and has its own small runtime over nine system calls.
* The engine is doomgeneric's and its Doom ancestry (GPL-2.0); `midifile.c` and `mus2mid.c` from Chocolate Doom; say so.
* EXAMPLE: `cd / && doom`; `tests/run doom` runs it for ten seconds and counts its disk commands.
* SEE ALSO: rach2(7), console(4).
