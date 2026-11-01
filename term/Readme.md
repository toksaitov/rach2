# term

`term` is the terminal that runs in user space on the whole framebuffer, where coding agents and other full-screen programs draw in color as they do on a desktop terminal. It runs a login or a shell on a pty, parses what the program writes with [libtsm](../vendor/libtsm/Readme.md) into a screen of cells, draws the cells with Unifont, and gives the program the keys of the console's keymap in xterm's and Kitty's encodings, under `TERM=xterm-256color`. The kernel console, `TERM=rach2`, stays beneath it and comes back when `term` exits.

## Running

    term [-l | -H] [-c FILE] [PROGRAM [ARG...]]
    term -V

`term` runs PROGRAM, found on `PATH`, or with no PROGRAM the shell `SHELL` names, `/bin/sh` when it is unset, on a pty the size of the screen, with `TERM=xterm-256color` and `COLORTERM=truecolor`. `-l` runs `/bin/login` instead and takes no PROGRAM; [init](../utils/Readme.md) starts `term -l` as the console session on a boot with `rach2.console=term`, which the sealed disk's menu carries. It takes the framebuffer and the console's keyboard, and gives both back when the program exits. Its own keys are below, under Keys, scrollback and selection. `-H` runs the program on a host pty with no screen, for tests, as below. `-c FILE` reads FILE in place of `/etc/term.conf`, and `-V` prints `libtsm` and its version.

`term` exits with the program's status, or 128 and the number of the signal that ended it, as a shell reports it; under `-l` with 0 once the login session has ended, however it ended, since its status is the last command's of the shell and says nothing of `term`, and init reads a status other than 0 as `term`'s failure ([`init(8)`](../man/man8/init.8)); with 1 when it cannot start, or when SIGTERM, SIGHUP, SIGINT or SIGQUIT ends it, which hangs the program up; with 2 for a wrong usage; and with 0 after `-V` and `-h`.

### /etc/term.conf

Each line is `NAME = VALUE`, blank, or a comment that begins with `#`; blanks around the name and the value do not count. A file that does not exist leaves every setting at its default. A line with an unknown name or a bad value is reported on stderr, as `term: FILE:LINE: NAME: what is wrong`, and skipped.

| Name | Value | Default |
| --- | --- | --- |
| `font` | the packed font's absolute path | `/usr/share/term/unifont.font` |
| `scale` | the font's scale, 1 or 2 | 1 |
| `scrollback` | lines of history kept above the screen, 0 to 100000 | 1000 |

### term -H

`term -H` starts the program on a pty of 80 by 24 cells, parses its output onto libtsm's screen as `term` does, and answers commands read from standard input, one to a line, with one line on standard output, or with more for `dump`. TEXT takes the escapes `\\`, `\e`, `\r`, `\n`, `\t`, `\a`, `\b` and `\xHH`.

| Command | Answer |
| --- | --- |
| `send TEXT` | `ok` once TEXT is queued for the program's input |
| `wait TEXT` | `found ROW` once row ROW, from 0, shows TEXT; `missing` when the output ends without it |
| `dump` | `screen COLS ROWS X Y VISIBLE`, the cursor's column, row and whether it shows, then each row's text in UTF-8 without its trailing blanks |
| `cell X Y` | `cell X Y WIDTH CHARS fg=RRGGBB bg=RRGGBB` and the attributes set of `bold dim italic underline blink inverse protect`; CHARS is `-` for a blank cell or the code points as `U+XXXX` joined by `+`, and WIDTH is 0 for a wide character's right half |
| `resize COLS ROWS` | `ok` once the screen and the pty have the size, which sends SIGWINCH |
| `key EVENT...` | `sent TEXT`, escaped, the bytes the events sent the program; `+CODE` presses a key, `-CODE` releases it and `CODE` does both, with Linux's `KEY_*` codes |
| `keymap TABLE CODE VALUE` | `ok` once the keymap's entry is set, as KDSKBENT sets the console's; numbers in C's notation |
| `string N TEXT` | `ok` once function string N is TEXT, as KDSKBSENT sets the console's |
| `locks [N]` | `locks N`, the locks' KDGKBLED bits, after setting them to N when it is given |
| `clipboard` | `clipboard TEXT`, what a selection or OSC 52 put there, escaped |
| `exit` | `exit STATUS` once the program has exited and its output has ended |

A command that fails answers `error` and what went wrong. `wait` and `exit` hold the commands after them until they answer. At the end of standard input `term -H` closes the master, which hangs the program up, sends it SIGHUP, reaps it and exits with its status.

    $ printf 'send echo hi\\r\nwait hi\nsend exit 3\\r\nexit\n' | term -H
    ok
    found 0
    ok
    exit 3

## Interfaces

[`term.h`](term.h) declares the parts and the calls between them, a section for each part and its file; every call that can fail returns 0 or a negative errno value, as libtsm's do:

| Part | File | What it owns |
| --- | --- | --- |
| The program | `main.c` | the command line and the loop, which owns every dispatch |
| The pty | `pty.c` | the child, its pty and the queue of its input |
| The screen | `screen.c` | every call into libtsm but drawing's |
| The font | `font.c` | the packed Unifont, at 1x or 2x |
| Drawing | `draw.c` | the framebuffer and the back buffer |
| Selection | `select.c` | the selection from the keyboard, the clipboard and paste |
| Keys | `keys.c` | the console's keyboard through its keymap |
| The translator | `keymap.c` | the kernel's `rach2/sys/keymap.c`, compiled into `term` |
| Host mode | `host.c` | `term -H` |

## Design

### The pty, the screen and the loop

The loop in `main.c` polls four descriptors: a pipe the signal handler writes each caught signal to, the pty's master, the console and `/fb`. In each round it reads `/fb`'s event, then the keys, then at most `TERM_BURST`, 64 KiB, of the program's output, so that a key typed while the program floods the screen reaches it after one burst. It draws a frame after each round that changed the screen. SIGCHLD ends the loop with the program's status; SIGTERM, SIGHUP, SIGINT and SIGQUIT end it with 1. Either way `term` gives the keyboard back, then the screen, and closes the master, which sends SIGHUP to the program's foreground group.

The pty. `pty.c` opens the master with `posix_openpt`, close-on-exec, and forks. The child becomes a session leader, opens the slave with `O_NOCTTY`, takes it as its controlling terminal with TIOCSCTTY, sets its size with TIOCSWINSZ on the slave, since macOS refuses TIOCSWINSZ with ENOTTY on a master whose slave has never been opened, puts it on descriptors 0 to 2 and executes the program, with the signals `term` catches set back to their defaults. A master reads end of file while no slave is open (`rach2/sys/tty/pty.c`, `ptm_read`), so the parent reads nothing from it until the child has executed: it waits on a close-on-exec pipe that the exec closes, and on which a failed step writes its errno. The master is then made nonblocking. Input to the program goes through a queue, at most 1 MiB, of which each write sends what the master takes; the loop polls for POLLOUT while the queue holds bytes. A later resize is TIOCSWINSZ on the master, which sends SIGWINCH to the slave's foreground group itself. The end of the output is a read of 0 bytes, or EIO, as Linux reports it.

The screen. `screen.c` makes every call into libtsm but drawing's, which reads the screen it draws: `tsm_screen_draw`, the cursor and the scrollback's position. The parser's replies, such as the answer to a cursor position report, go to the pty's queue. The bell sets a flag the loop clears when it shows the bell. OSC 52 replaces the clipboard with the decoded text, which may be at most 1 MiB; a query, `?`, is not answered, so that no program reads what another put there. Mode 2026's hold is read with `term_screen_held()`: while it is on and a frame waits, the loop wakes every `TERM_HOLD_POLL_MS`, 20 ms, or sooner when `term_draw_timeout` says so, so that drawing can end a hold that outlasts its bound.

`/fb`'s events. FB_EVENT_LOST, when a program `term` started takes the screen, stops the keys and the frames; FB_EVENT_REGAINED forgets the keys seen held and draws the whole screen again. When the cells the screen holds change, as when the font's scale does, the loop resizes libtsm's screen and the pty.

`term -H` runs the same pty and screen in `host.c`, with standard input for commands in place of the keys and no `/fb`. It reads the cells through `term_screen_snapshot()`, which copies them with `tsm_screen_draw` with the cursor hidden, so that the cursor's cell keeps its own attributes. The copy resets libtsm's record of what changed, which is why `term` on a screen never takes one.

### Drawing

`font.c` reads the packed Unifont of [`unifont.h`](unifont.h), installed as `/usr/share/term/unifont.font` (`TERM_FONT_PATH`), whole into memory, about 3.7 MB, and checks it with `unifont_open`. The font is drawn at a scale of 1 or 2: a cell is 8 by 16 pixels or 16 by 32, so the Lenovo's 1366 by 768 screen has 170 by 48 cells or 85 by 24, and the pixels left past the last cell are black. `term_draw_rescale` changes the scale and with it the screen's size in cells, which the loop gives libtsm and the pty, sending the child SIGWINCH; it draws every cell again and keeps the scale in the settings file as the line `scale = N`, replacing the file's `scale` line and keeping its others, through a file of the same name with `.new` appended, renamed over it.

`term_draw_open` opens `/fb` close-on-exec, reads the framebuffer with FB_GETINFO and enters its native mode with FB_MODE_NATIVE ([`<rach2/fb.h>`](../rach2/include/rach2/fb.h)); it takes RGB at 15, 16, 24 or 32 bits a pixel, with each channel where FB_GETINFO puts it, and refuses anything else with ENODEV. Frames are drawn into a back buffer in RAM of the framebuffer's own layout, so that a byte's offset in it is its offset for `pwrite`. `term_draw_init` does the same on any descriptor, which is how the host test draws into a file.

`term_draw_frame` calls `tsm_screen_draw`, which reports every cell with the age of its last change, and draws only the cells newer than the last frame and the cursor's old and new cells. It draws every cell, and the margins, after `term_draw_redraw` or an age set to 0, as when the screen comes back, after a change of scale, at the bell's start and end, and after a write that failed. It marks, for each row of cells, the first and last pixel column it drew. A row whose change is narrower than a quarter of the screen is written a pixel row at a time; a wider one is written as one range from its first changed byte to its last, joined with the rows beside it, since the bytes between are already on the screen; a whole frame is one `pwrite`.

A cell's glyph is rendered at scale 1 and kept in a cache of 1024 glyphs, found by its code points and shape, and scaled as its pixels are written. The glyphs of the base and of each combining mark are put over each other, since `tsm_screen_draw` gives a cell all its code points; a character the font lacks is drawn as U+FFFD. Bold widens each stroke by a pixel, italic slants the rows by up to 3 pixels at the top, underline fills the glyph's last row, dim draws the foreground halfway to the background, and inverse swaps the two; blink is drawn steady. The colors are libtsm's 24-bit ones, with the palette and bold's bright colors already applied. A wide character's glyph spans its two cells: the second cell, of width 0, is drawn by drawing the first again, and a width-0 cell that a narrow character left behind is drawn empty. A glyph 16 pixels wide in a cell libtsm gives a width of 1 is cut to its left half, and so is a wide character in the last column.

libtsm marks the cursor by inverting its cell; drawing undoes that and draws the shape the child chose with DECSCUSR: a block, which inverts the cell, an underline of two rows, or a bar of two columns, each doubled at 2x. The blinking shapes are drawn steady. A hidden cursor (DECTCEM) is not drawn, and while scrollback is shown the cursor moves down with the screen's rows. The bell shows every cell in inverse colors for 100 ms (`TERM_DRAW_BELL_MS`). While the child holds the screen with mode 2026 the last frame stays; after 250 ms (`TERM_DRAW_HOLD_MS`) drawing ends the hold with `term_screen_release` and draws, so a child that dies holding it does not freeze the screen. The loop shows the bell with `term_draw_bell` when libtsm reports it; `term_draw_timeout` gives the loop the milliseconds until the bell's end or the hold's bound, when a frame is due although the child wrote nothing.

### Keys, scrollback and selection

`keys.c` opens `/dev/console`, puts its keyboard in K_MEDIUMRAW with KDSKBMODE and its terminal settings in raw mode, so that every byte of a key code reaches `term` as it is typed, and gives both back at the end. K_MEDIUMRAW sends a key code below 128 as one byte, with 0x80 set on release, and a higher one as three; a repeat comes as another press, so `term` keeps a bit for each key held and takes a press of a key already down as a repeat. The set is cleared when the screen comes back from a program `term` started, which may have taken keys up that `term` never saw.

Each key goes through the console's own translator, [`rach2/sys/keymap.c`](../rach2/sys/keymap.c), which `keymap.c` compiles into `term`, so the console and `term` give a key the same character from the same keymap. The translator reads the tables through a callback, which asks the console for the key's entry with KDGKBENT as the key is pressed; a key's function string is read with KDGKBSENT when it is pressed, and the accents with KDGKBDIACRUC while a dead key waits. A map that `loadkeys` loads while `term` runs therefore takes effect at the next key. KDGKBENT gives K_HOLE, not K_NOSUCHMAP, for a key of a table that does not exist, so a hole is checked against the table's key 0. The locks are read with KDGKBLED before each key and written back with KDSKBLED when a key changes them, with the default flags kept, so that `setleds` and `term` agree and the LEDs follow.

A key becomes libtsm's key event, `tsm_vte_handle_key`, which encodes it as xterm does or by the Kitty protocol under the flags the program set. The keysym is XKB's: a character's own, the cursor and editing keys', the function keys' for each string that is the built-in one, and for each that is xterm's sequence of a function or editing key, with the modifiers of its parameter, as the maps' `xterm-strings` give Shift+F1 as F13's `ESC [ 1 ; 2 P`, Menu for the Compose key, and the keypad's, which with Num Lock on, reversed by Shift, are its digits and point. libtsm also gets Caps Lock and Num Lock, which the Kitty protocol reports with every key, and Num Lock keeps the keypad numeric in application mode. Any other function string is sent as it is. The characters libtsm needs besides the key's own come from the same translator run on a copy of the state through a view of the tables: the character before Ctrl and Alt, which xterm sends after ESC for Alt; the character with no modifier in the layout in use, Kitty's key code; that in the base layout, table 0 with no lock, Kitty's base layout key, which on every generated map is US; and the base layout's character with Shift alone, from which Ctrl makes its control code. So Ctrl+C sends ETX on a Cyrillic layout, and the Kitty protocol reports it as `CSI 1089::99;5u` with flags 1 and 4. Kitty's flag 2 reports a key's repeats and its release with the key its press sent. Backspace sends DEL and Ctrl+Backspace BS.

`term` keeps a few keys of its own, all of them keys the key table ([`tests/host/keytable.txt`](../tests/host/keytable.txt)) has send nothing or act locally, so that every other key reaches the program as xterm sends it:

| Keys | What `term` does |
| --- | --- |
| Shift+Page Up, Shift+Page Down | show the scrollback a page further back or forward; on the alternate screen, which keeps none, the program gets them |
| Ctrl+Shift+Page Up, Ctrl+Shift+Page Down | draw the font at 2x or at 1x, and keep the scale in `/etc/term.conf` |
| Print Screen | turn the selection mode on, and off again |
| Shift+Print Screen | paste the clipboard |

The scrollback holds the lines that scroll off the top of the main screen, as many as `scrollback` says; libtsm keeps none for the alternate screen, and shows the screen again when a program switches to it. Any key that sends the program something shows the screen itself again.

The selection mode, in `select.c`, takes every key while it is on. Its cursor starts on the screen's and is drawn as a selected cell: the cursor keys and `h`, `j`, `k` and `l` move it, Home and `0` and End and `$` take it to a row's ends, Page Up and Page Down move it a page, and moving past the top or the bottom row scrolls the scrollback. Space or `v` marks the start, after which libtsm shows the cells from the mark to the cursor in inverse colors; Enter or `y` copies them and leaves, and Escape or `q` leaves without a copy. Leaving shows the screen again.

There is one clipboard, the screen's, which a selection and OSC 52 both replace. A paste sends it to the program with each LF as CR, as Enter sends, and with ESC left out, so that pasted text can neither end mode 2004's brackets nor send a sequence; under mode 2004 libtsm puts it between `CSI 200 ~` and `CSI 201 ~`. The clipboard's bytes are overwritten before they are freed, and they are freed when `term` exits, at the end of the session it ran.

`showkey` reads the console's keyboard in a mode of its own, so it is run on the kernel console, with `term` not running: under `term` the two would read the same keys. `setleds` and `kbdrate` act on the console that is their standard input, so under `term`, whose program has a pty, they are run as `setleds +num < /dev/console`; `term` takes the locks they set at the next key.

## Building

`make term` builds `term` for the architecture `ARCH` selects, into `build/term/term` or `build/x86_64/term/term`, and the root `make` builds it too; `make term-host` builds it for the host, into `build/host/term/term`. [`mk/term.mk`](../mk/term.mk) has the rules, and [`sources.mk`](sources.mk) the one list of sources. The dialect is C99, as `libtsm.h`'s is, and both builds find `<rach2/fb.h>` in `rach2/include`, as Doom's port does. The Rach2 build compiles with the musl wrapper, links the architecture's `libtsm.a` before musl's `libc.a` and runs `elfcheck` on the program; the host build compiles with `HOSTCC` and `HOSTCFLAGS` and links the host's `libtsm.a`. Each object depends on the headers it includes and on its libtsm's stamp, which stages the patched `libtsm.h`.

### On Rach2

On Rach2, `make -f guest.mk` in a writable copy of `/src/term`, or in `/src/term` itself as root, builds `term` with pdpmake and the staged TinyCC through `/bin/cc`, on either architecture. The images that stage the kernel's sources (`os-real`, `os-real-gnu`, `grub-test` and `rach2-metal-disk`) stage this directory as `/src/term`, with libtsm's patched sources beside it as `vendor/libtsm.tar.bz2`, which `build_libtsm.sh` packs, and their list as `vendor/libtsm.mk`, a copy of [`vendor/libtsm/sources.mk`](../vendor/libtsm/sources.mk).

`keymap.c` includes the kernel's translator by its path from this directory, `../rach2/sys/keymap.c`, so the build needs `/src/rach2` beside this tree, as every image that stages one stages the other; a writable copy is made of both.

[`guest.mk`](guest.mk) unpacks the archive into `vendor/libtsm` as its first prerequisite, compiles the files of `sources.mk` and of libtsm's list with one set of flags, and links the objects with musl's `libc.a` to `term.new`, which it renames to `term`. A POSIX makefile has one rule for every `.c`, so term's files take libtsm's flags, `-Dwcwidth=tsm_wcwidth` among them. TinyCC does not define `__SIZEOF_POINTER__`, which libtsm's `shl-macro.h` tests, so the makefile gives it from `uname -m`. TinyCC 0.9.27 compiles the ten files of `sources.mk` and libtsm's eight, with the whole patch series, without a warning or a change to the sources on both architectures, and refuses nothing. `tests/run style` checks with `tools/verify/guestcheck.c` that the object list is the two lists, that the archive is unpacked first and that the link goes through `term.new`.

`guest.mk` makes the program and not its packed font, which is the same on both architectures and which the build host makes from Unifont's text with [`mk/unifont.mk`](../mk/unifont.mk); making it here would put that text, 1.7 MB compressed, in every image that stages the sources. The program built here reads the font at `/usr/share/term/unifont.font`, or where the `font` line of the file `-c` names puts it, so `./term -c FILE` started at the console's shell, on a machine with a framebuffer, runs a shell in it and gives the console back when that shell exits.

## Tests

[`tests/host/term_hosttest.c`](../tests/host/term_hosttest.c) runs `term -H` in a child with pipes on its standard input and output, with `sh -c` and a script as the program: the program's environment, a line sent to it, its output's colors, cursor and wide characters on the screen, the parser's reply to a cursor position report, a resize seen by `stty size`, its exit status, the hang-up at the end of standard input, a program that cannot be run, and `/etc/term.conf` with each kind of bad line. It compiles libtsm from the extracted i686 tree, as `build_libtsm.sh` does.

### Keys

[`tests/host/term_keys_hosttest.c`](../tests/host/term_keys_hosttest.c) runs `term -H` as `term_hosttest.c` does, on a shell script that puts its pty in raw mode, writes the modes a group of cases needs and reads its input away, and gives `term` keys with the `key` command, which answers with the bytes `term` wrote to the pty. It runs every case of the key table but those of its `tty` modes, which are the kernel's line discipline's, under DECCKM and DECKPAM as each case's modes say and with Num Lock and Caps Lock set first by `locks`; the Kitty protocol's cases: Shift+Enter with flag 1, Russian Ctrl+C on a two-layout map, made with `keymap` as ckbcomp writes one, with flag 1 and with flags 1 and 4, Ctrl+Shift+C there with its shifted key, and the repeat and release events of flag 2; a map's entries and strings changed while `term` runs, a press of a key already down taken as a repeat, and Caps Lock's lock; a child that reads the bytes of a Cyrillic letter and of Shift+Enter from its pty and prints them; the scrollback a page at a time, shown again by a key for the program and left to the program on the alternate screen; and the selection of one line and of two, the keys the mode takes, and paste, bracketed under mode 2004.

### Drawing

[`tests/host/term_draw_hosttest.c`](../tests/host/term_draw_hosttest.c) draws into a file of a framebuffer's layout, at 32, 24, 16 and 15 bits a pixel, and checks each pixel against a value it works out from the format. In place of libtsm's screen it has a model that reports its cells as `tsm_screen_draw` does, with ages, so it can check that a frame writes the cells that changed and leaves the others alone. It checks the margins, bold, italic, underline, dim and inverse, a wide character and the cell it leaves when replaced, a combining mark, U+FFFD for a missing glyph, the three cursor shapes, a hidden cursor and one moved by scrollback, the bell, mode 2026's hold and its bound, and a change of scale with the settings file it rewrites.

### esctest2 and the Unicode tests

[`tests/run esctest2`](../vendor/esctest2/Readme.md) runs the pinned upstream Python driver on `term -H`'s pty on Linux and macOS. The case excludes Windows, where `term -H` has no POSIX pty. The C controller uses `dump` and `cell` for screen inspection after a CPR round trip, while protocol queries keep their real replies. It checks every result against an exact list of known failures with reasons. Changes to the list may only remove entries, and the required terminal features cannot be exempted. Most accepted failures are for what libtsm lacks: left and right margins, the rectangle operations, DECRQSS, xterm's window operations and changes to the palette.

[`term_unicode_hosttest.c`](../tests/host/term_unicode_hosttest.c), part of the host suite, feeds the actual screen layer with decomposed accents, Cyrillic, CJK, variation selectors, emoji with modifiers, flags and ZWJ sequences at every UTF-8 split point. It replays task 12's `TERM=xterm-256color` recording of Claude Code, checks the check mark, rocket and CJK cells while they are visible, and compares the final cells, colors and cursor with input fed whole. The recording contains the agent's CJK and rocket; the other vectors are explicit additions, rather than claims about text in the recording.

Combining marks and variation selectors join the preceding cell. There is no grapheme clustering or emoji composition: the skin modifier occupies two cells of its own, a flag is two regional indicators of one cell each, and the woman, ZWJ and laptop sequence occupies four cells, with the ZWJ attached to the woman. Variation selector 16 does not change the base width. Unifont draws monochrome glyphs and substitutes U+FFFD for a glyph it lacks. These limits are checked as the current cell contract; the end run records any further drawing defects here.
