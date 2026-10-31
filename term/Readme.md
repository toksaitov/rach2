# term

`term` is the terminal that runs in user space on the whole framebuffer, where coding agents and other full-screen programs draw in color as they do on a desktop terminal. It runs a login or a shell on a pty, parses what the program writes with [libtsm](../vendor/libtsm/Readme.md) into a screen of cells, draws the cells with Unifont, and gives the program the keys of the console's keymap in xterm's and Kitty's encodings, under `TERM=xterm-256color`. The kernel console, `TERM=rach2`, stays beneath it and comes back when `term` exits. Today its pty, its screen, its loop, its drawing and `term -H` are written; the keys, the part below that says so, are being written, and until they are `term` on a screen fails at its start.

## Running

    term [-l | -H] [-c FILE] [PROGRAM [ARG...]]
    term -V

`term` runs PROGRAM, found on `PATH`, or with no PROGRAM the shell `SHELL` names, `/bin/sh` when it is unset, on a pty the size of the screen, with `TERM=xterm-256color` and `COLORTERM=truecolor`. `-l` runs `/bin/login` instead and takes no PROGRAM. It takes the framebuffer and the console's keyboard, and gives both back when the program exits. `-H` runs the program on a host pty with no screen, for tests, as below. `-c FILE` reads FILE in place of `/etc/term.conf`, and `-V` prints `libtsm` and its version.

`term` exits with the program's status, or 128 and the number of the signal that ended it, as a shell reports it; with 1 when it cannot start, or when SIGTERM, SIGHUP, SIGINT or SIGQUIT ends it, which hangs the program up; with 2 for a wrong usage; and with 0 after `-V` and `-h`.

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
| `clipboard` | `clipboard TEXT`, what OSC 52 set, escaped |
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
| Host mode | `host.c` | `term -H` |

`keys.c` is a stub whose calls fail with ENOSYS, and `select.c`'s selection mode is never on.

## Design

### The pty, the screen and the loop

The loop in `main.c` polls four descriptors: a pipe the signal handler writes each caught signal to, the pty's master, the console and `/fb`. In each round it reads `/fb`'s event, then the keys, then at most `TERM_BURST`, 64 KiB, of the program's output, so that a key typed while the program floods the screen reaches it after one burst. It draws a frame after each round that changed the screen. SIGCHLD ends the loop with the program's status; SIGTERM, SIGHUP, SIGINT and SIGQUIT end it with 1. Either way `term` gives the keyboard back, then the screen, and closes the master, which sends SIGHUP to the program's foreground group.

The pty. `pty.c` opens the master with `posix_openpt`, close-on-exec, sets its size with TIOCSWINSZ before the program starts, and forks. The child becomes a session leader, opens the slave with `O_NOCTTY`, takes it as its controlling terminal with TIOCSCTTY, puts it on descriptors 0 to 2 and executes the program, with the signals `term` catches set back to their defaults. A master reads end of file while no slave is open (`rach2/sys/tty/pty.c`, `ptm_read`), so the parent reads nothing from it until the child has executed: it waits on a close-on-exec pipe that the exec closes, and on which a failed step writes its errno. The master is then made nonblocking. Input to the program goes through a queue, at most 1 MiB, of which each write sends what the master takes; the loop polls for POLLOUT while the queue holds bytes. A later resize is TIOCSWINSZ on the master, which sends SIGWINCH to the slave's foreground group itself. The end of the output is a read of 0 bytes, or EIO, as Linux reports it.

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

Task 22 writes this section.

## Building

`make term` builds `term` for the architecture `ARCH` selects, into `build/term/term` or `build/x86_64/term/term`, and the root `make` builds it too; `make term-host` builds it for the host, into `build/host/term/term`. [`mk/term.mk`](../mk/term.mk) has the rules, and [`sources.mk`](sources.mk) the one list of sources. The dialect is C99, as `libtsm.h`'s is, and both builds find `<rach2/fb.h>` in `rach2/include`, as Doom's port does. The Rach2 build compiles with the musl wrapper, links the architecture's `libtsm.a` before musl's `libc.a` and runs `elfcheck` on the program; the host build compiles with `HOSTCC` and `HOSTCFLAGS` and links the host's `libtsm.a`. Each object depends on the headers it includes and on its libtsm's stamp, which stages the patched `libtsm.h`.

### On Rach2

Tasks 20 and 27 write this section.

## Tests

[`tests/host/term_hosttest.c`](../tests/host/term_hosttest.c) runs `term -H` in a child with pipes on its standard input and output, with `sh -c` and a script as the program: the program's environment, a line sent to it, its output's colors, cursor and wide characters on the screen, the parser's reply to a cursor position report, a resize seen by `stty size`, its exit status, the hang-up at the end of standard input, a program that cannot be run, and `/etc/term.conf` with each kind of bad line. It compiles libtsm from the extracted i686 tree, as `build_libtsm.sh` does.

### Drawing

[`tests/host/term_draw_hosttest.c`](../tests/host/term_draw_hosttest.c) draws into a file of a framebuffer's layout, at 32, 24, 16 and 15 bits a pixel, and checks each pixel against a value it works out from the format. In place of libtsm's screen it has a model that reports its cells as `tsm_screen_draw` does, with ages, so it can check that a frame writes the cells that changed and leaves the others alone. It checks the margins, bold, italic, underline, dim and inverse, a wide character and the cell it leaves when replaced, a combining mark, U+FFFD for a missing glyph, the three cursor shapes, a hidden cursor and one moved by scrollback, the bell, mode 2026's hold and its bound, and a change of scale with the settings file it rewrites.

### esctest2 and the Unicode tests

Task 24 writes this section.
