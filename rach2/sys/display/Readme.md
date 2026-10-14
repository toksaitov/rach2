# Display

`sys/display/` draws the console's text and the 320x200 picture a program shows through `/fb`. The text console, `vga_text.c` behind `textcon.h`, keeps the screen's cells, a scrollback and the cursor, and writes them to the VGA's text memory or, when the loader left a framebuffer, to `fbcon.c`, which draws them in pixels. `vga_gfx.c` puts the VGA in mode 13h for `/fb`, or on a framebuffer console keeps the picture in memory for `fbcon.c` to draw. `vga_font.c` is the console's font. The interpreter of the console's escape sequences is `sys/kio.c`, and what is typed at the console is described in [the terminal Readme](../tty/Readme.md).

## Interfaces

Programs see the screen as [console(4)](../../../man/man4/console.4) describes it: the escape sequences of the `rach2` terminfo entry ([`vendor/gnu/rach2.ti`](../../../vendor/gnu/rach2.ti)), the font and its stand-ins, the answers to queries, and the window size. `/fb` is described in [open(2)](../../../man/man2/open.2), [read(2)](../../../man/man2/read.2), [write(2)](../../../man/man2/write.2) and [ioctl(2)](../../../man/man2/ioctl.2).

Inside the kernel:

* `textcon.h`, defined by `vga_text.c`: the grid of `textcon_cols` by `textcon_rows` cells, `textcon_write_cell`, `k_scroll_down`, `k_cls`, the cursor (`textcon_place_cursor`, with `textcon_hold` and `textcon_release` around a write of many bytes), the colors, the scrollback (`k_scrollback_page_up`, `k_scrollback_page_down`, `k_scrollback_home`, `k_scrollback_end`) and the font (`textcon_load_font`).
* `fbcon.h`: `fbcon_init`, which the platform calls with the loader's framebuffer, and `fbcon_window`, which the platform defines in `sys/arch/x86/boot.c`.
* `fb.h`, defined by `vga_gfx.c`: the graphics mode of `/fb`, `fb_enter`, `fb_exit`, `fb_set_palette` and `fb_present`, and `gfx_current`, the mode in force.

The boot log records the console's choices:

* `console: framebuffer: 1366x768, 32 bits a pixel; the font at 2x, 85x24 cells`, or `console: framebuffer: not one this console draws; the screen stays as the loader left it`.
* `vga: font: loaded: code page 437 box drawing with Cyrillic` or `vga: font: kept: the adapter's code page 437`, and on a framebuffer `console: font: drawn: code page 437 box drawing with Cyrillic`.
* `cpu: page attributes: 0x0007040600070106; the framebuffer is mapped write-combining`, or the same line ending in `uncached`, or `cpu: page attributes: none; the framebuffer is mapped uncached`.

## Design

### The screen's terminal

The console terminal's sink and the kernel's console calls (`console_putc` in `sys/klog.c`) write the screen through `k_putc` in `sys/kio.c`. It runs each byte under the console's lock (`console_lock`: interrupts off, and a spinlock taken unless this processor has it already), since a keyboard's echo arrives from an interrupt and other processors write too. It interprets:

* CSI sequences, `ESC [` with up to eight numeric parameters: cursor motion `A` to `G`, `` ` ``, `d`, `H` and `f`; erase `J`, `K` and `X`; color `m`; save and restore `s` and `u`; and the queries `c` and `n`. A private sequence (`ESC [ ?`), one with an intermediate byte, and any other final byte, `r`, `h` and `l` among them, are read and ignored.
* `ESC 7` and `ESC 8`, which save and restore the cursor, and `ESC c`, which resets the colors and clears the screen. `ESC (` and its kin take one byte more. An OSC, DCS, APC, PM or SOS string is read to BEL, ESC or another control byte, or for 256 bytes, and is not drawn.
* UTF-8. A character is drawn as its place in the font's upper half (`vga_font_map` when the font was loaded, code page 437 otherwise); one the font lacks as a stand-in, a Latin letter without its accent, an ASCII likeness or a related box glyph; and one with no stand-in as `?`.
* Control bytes. BS, HT (stops every 8 columns), LF, which also returns the carriage, and CR move the cursor; NUL and BEL draw nothing; any other is drawn as `?`.

A byte written in the last column leaves the cursor there with the wrap pending, as terminfo's `xenl` says: the next printable byte begins the next line, and a cursor move, an erase, BS, HT, LF or CR cancels the wrap. A program can then fill the last row without the screen scrolling.

SGR takes 0; 1 and 22, bold, which sets the foreground's bright bit; 7 and 27, reverse; 30 to 37, 39 and 90 to 97 for the foreground; and 40 to 47, 49 and 100 to 107 for the background, which has eight colors. ANSI's yellow is the VGA's brown.

The answer to a query (`ESC [ ? 6 c`, `ESC [ 0 n`, or the cursor's position) waits in a buffer of 16 bytes until `tty_write` takes it with `k_take_answer` and gives it to the console's input.

### The text console

`vga_text.c` keeps the text in a history ring of 512 lines (`VGA_TEXT_SCROLLBACK_LINES`), each of up to 240 cells, a character byte and an attribute byte. Lines are numbered by a count that only grows. The live area, where output lands, is the last `textcon_rows` lines; the viewport is the lines on the screen, the live area unless the screen is scrolled back. Once the ring is full each new line replaces the oldest.

The VGA's text memory is 32 KiB at physical 0xB8000, 16384 cells, room for eight 80x25 screens. Each store there is an uncached bus cycle on hardware and an exit to the emulator under a hypervisor, so the driver writes each cell once:

* A byte of live output is one store, to the history and to the one cell that shows it.
* A scroll moves the display's start address (CRTC registers 0x0C and 0x0D, through ports 0x3D4 and 0x3D5) down one line and writes the new bottom line blank. When the window would pass the end of the memory, the rows that stay are copied to its front and the start address returns to 0: at 80x25, once every 180 scrolls.
* The cursor (CRTC registers 0x0E and 0x0F) counts cells from the start of the memory, so the window's start is added to it. Placing it takes four port writes, so a writer of many bytes takes a hold (`textcon_hold`, which nests) and the cursor is placed once, at the release.
* Only a viewport that moves, by a scroll back or a clear, is drawn whole (`textcon_sync`).

While the adapter is in a graphics mode the start address is left alone; leaving graphics mode clears the screen, which sets it again.

### Scrollback

The keys come from `sys/input.c`. While the debug shell reads input, Page Up and Page Down move the viewport a screen at a time, and Home and End go to the oldest line and to the live screen. In a session those keys go to the program as escape sequences, and Shift with Page Up or Page Down moves the viewport. Any other key in a session, and a typed character, Backspace or Enter in the debug shell, returns the screen to the live area. While the screen is scrolled back, output goes to the history alone; a new line that pushes the viewport's top line out of the ring moves the viewport to the oldest line. A clear (`k_cls`, which `ESC c` and the end of graphics mode call) empties the history.

### The font

`vga_font.c` is written by `tools/font/mkfont.sh` from GNU Unifont 18.0.01, under the SIL Open Font License 1.1, and is not edited by hand. Its upper half keeps code page 437's box drawing, shades and blocks, with Cyrillic letters in place of the Latin and Greek ones. At boot `k_font_init` loads its 98 glyphs that differ from code page 437 (`VGA_FONT_GLYPHS`) into the adapter's character memory, plane 2 opened at 0xA0000 with 32 bytes a character, of which an 8x16 glyph uses 16, and reads them back (`vga_font_load` in `vga_gfx.c`). When every byte reads back, `kio.c` maps text through `vga_font_map`; otherwise the adapter's own code page 437 stays. `vga_font_all` is the whole font, 256 glyphs with the control codes blank, for the framebuffer, which has no font of its own.

### The framebuffer console

`k_main` in `sys/arch/x86/boot.c` calls `fbcon_init` before the shared boot when the boot record, Multiboot's or multiboot2's, describes a framebuffer of direct color at 8 to 32 bits a pixel or of 8-bit indexed color, as GRUB leaves one under UEFI. `fbcon_init` returns -1, and the console stays on the VGA, when the screen has fewer than 640x384 pixels (80 columns and 24 rows of the font at scale 1), when its pitch is shorter than a row of pixels, or when `fbcon_window` cannot reach it.

The glyph, 8 pixels by 16, is drawn at the largest whole scale at which the screen still holds 80 columns and 24 rows, so each pixel of the font is a square of whole pixels, and the console has as many cells as fit at that scale, at most 240 by 64 (`TEXTCON_COLS_MAX`, `TEXTCON_ROWS_MAX`), centered. A 1366x768 screen has scale 2 and 85x24 cells, and a 1280x800 one scale 2 and 80x25. A cell's sixteen colors are the VGA's text palette, converted to the screen's pixel fields, or on an indexed screen the nearest entries of the loader's palette.

`fbcon.c` stands in for the adapter. `fbcon_cells`, 16384 cells, takes the stores `vga_text.c` makes, and `fbcon_set_start` and `fbcon_set_cursor` take the start address and the cursor. `drawn` records what each screen cell shows, and a cell is drawn only when the cell the start address selects for it differs, or the cursor came or went, since each of its pixels is a store to the display's memory. After a scroll a cell is drawn only where the line moving into it differs from the line drawn there, so a blank cell moving onto a blank one is not drawn. The cursor is rows 13 and 14 of its cell, the two lines the VGA's text mode underlines a cell with.

`fbcon_window` gives the console the framebuffer's address:

* Until the kernel's own page tables are in force, the loader's view of memory: the physical address itself on i686, where paging is off, and on x86-64 its address in the direct map of the first 4 GiB that `loader.s` builds.
* After that, a mapping in the kernel's MMIO region (`map_mmio_typed`), made the first time the console draws.
* For a framebuffer that reaches past 4 GiB, no address: `fbcon_init` fails and the screen stays as the loader left it.

The mapping is write-combining where the processor has a page attribute table, and uncached otherwise. `pat_setup` (`sys/arch/x86/pat.c`) writes IA32_PAT with entry 1 write-combining in place of write-through and the other seven at their reset types, by the SDM's procedure for a memory type change on a running processor. The boot processor runs it in `platform_cpu_init`, before the kernel's tables map the framebuffer, and each started processor runs it first in `x86_ap_entry`, before it can draw. `PTE_WC`, PWT alone (0x008 on both architectures), selects entry 1, and only `fbcon_window` uses it. The console and the `/fb` picture only write the framebuffer, and write-combining merges their stores into bursts, where uncached memory takes a bus transaction for each.

The console terminal's window size is the cells (`k_console_size`, read once by `tty_console_init`), and the `rach2` entry gives no size, so a program takes the screen's from TIOCGWINSZ.

### `/fb` and mode 13h

One process at a time owns the display, recorded by thread group id in `fb_owner_pid` (`sys/syscall.c`), so any of its threads may use it.

* An open of `/fb` (`open_framebuffer` in `sys/syscall_fs.c`) fails with EBUSY when another process owns the display. Otherwise it enters mode 13h (`fb_enter`), makes the caller the owner, and empties the raw scancode ring of `sys/kbd.c`.
* `framebuffer_ioctl` in `sys/syscall.c` takes `FB_MODE_13H` (0x4601), `FB_MODE_TEXT` (0x4602) and `FB_SETPALETTE` (0x4603), and returns ENOTTY for any other request. A caller other than the owner gets EBUSY, with a descriptor inherited by fork or kept after the display was released, except for `FB_MODE_13H` on an unowned display, which enters mode 13h and makes the caller the owner. `FB_MODE_13H` from the owner in graphics mode keeps the picture and the palette. `FB_MODE_TEXT` returns to text mode and clears the ownership. `FB_SETPALETTE` reads 768 bytes, 256 colors of 8-bit red, green and blue.
* The display returns to text mode when the owner closes the last descriptor of its open file description (a forked child's close does not), and when the owner exits (`exit_release_session` in `sys/syscall_proc.c`).
* A write copies at most 64000 bytes to the picture, in graphics mode only, and calls `fb_present`. A console read by the owner returns the raw scancodes without waiting.

On the VGA, `fb_enter` saves the text font from plane 2, programs mode 13h's registers from their tables, and clears the 64000 bytes at 0xA0000. `fb_exit` programs mode 3, loads the sixteen text colors into the DAC, restores the saved font, and clears the text screen. A palette goes to the DAC shifted right by 2, since the DAC's values have 6 bits. Both run with interrupts off.

On a framebuffer console there may be no VGA, as on a machine booted by UEFI, and the VGA's aperture at 0xA0000 may alias the screen the console draws on, so `vga_gfx.c` leaves the hardware alone and the picture is memory of its own (`gfx_shadow`, 64000 bytes). `fbcon_gfx_begin` paints the screen black and centers the picture at the largest whole scale at which 320x200 fits, 3 on a 1366x768 screen, lowered if need be until a scaled line fits its line buffer of 16384 bytes. Until the first palette the picture's first sixteen colors are the console's and the rest black; `fbcon_gfx_palette` converts a palette to the screen's pixels. Each write is drawn by `fbcon_gfx_frame`, which builds each scaled line once and copies it as many times as the scale. No cell is drawn while the picture has the screen; `fbcon_gfx_end` paints the screen black and draws every cell again, and `fb_exit` then clears the text screen.

## Limits

* `/fb` has one mode, 320x200 at a byte a pixel through a palette of 256 colors. `GFX_MODE_VBE` in `fb.h` is a mode with no driver.
* A write to `/fb` tests for graphics mode and not for ownership: a descriptor inherited by fork, or kept after the display was released, writes into the picture of whichever process owns the display.
* The framebuffer console draws only on a framebuffer below 4 GiB, of at least 640x384 pixels, of direct color at 8 to 32 bits a pixel or 8-bit indexed; with any other the console stays on the VGA's text mode, which a machine booted by UEFI may lack.
* The console has no scroll region and no line or character insertion, so a program redraws where a terminal would scroll.
* The background has eight colors.
* The scrollback is 512 lines, the screen's own included, and a clear empties it. Output written while the screen is scrolled back shows when it returns to the live area.
* The window size is set once at boot. TIOCSWINSZ on the console changes what TIOCGWINSZ reports and leaves the screen as it is.

## Tests

The host tests run with `tests/run host`:

* `tests/host/vga_text_hosttest.c`: the driver against a model adapter whose stores and port writes are counted: a byte as one cell, a scroll as one line and the start address, the copy once in 180 scrolls, the redraw by a scroll back and a clear, the held cursor placed once, and the pending wrap.
* `tests/host/fbcon_hosttest.c`: the console drawn into a framebuffer in memory: glyphs in their colors, the cursor's two lines, a Cyrillic letter reached through UTF-8, the scale and the cells within 240 by 64, pixels of 32, 24, 16 and 8 bits, the centering, what a scroll and a clear draw, the scrollback, and the mode 13h picture's scale and place.
* `tests/host/vga_gfx_hosttest.c`: the mode changes with a keyboard interrupt pending at each port access, and the font loaded into the character memory, read back, and not loaded in a graphics mode.

The suites, each run with `tests/run NAME`:

* `console`: the screen's answers to ttyprobe's queries.
* `uefi`: a machine booted by OVMF, with the console on its framebuffer. A dump of the screen must show the login prompt in the kernel's font, the page attribute table must read back with entry 1 write-combining, and Doom's picture must be on the screen in its palette's colors while it runs, and the console's colors again after it ends.
* `doom`: the game, which draws through `/fb`, on the VGA; it judges the game's memory and exit status and not the picture.
