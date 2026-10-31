# term

`term` is the terminal that runs in user space on the whole framebuffer, where coding agents and other full-screen programs draw in color as they do on a desktop terminal. It runs a login or a shell on a pty, parses what the program writes with [libtsm](../vendor/libtsm/Readme.md) into a screen of cells, draws the cells with Unifont, and gives the program the keys of the console's keymap in xterm's and Kitty's encodings, under `TERM=xterm-256color`. The kernel console, `TERM=rach2`, stays beneath it and comes back when `term` exits. Today `term` is a skeleton: `term -V` prints the version of libtsm it was built with, and the parts below are being written.

## Running

    term [-l | -H] [PROGRAM [ARG...]]
    term -V

`-l` runs `/bin/login` in place of the user's shell, `-H` runs the program on a host pty with no screen, for tests, and `-V` prints `libtsm` and its version. `term` reads `/etc/term.conf` as it starts. It exits 0 after `-V` and `-h`, 2 for a wrong usage, and 1 otherwise, since the loop is not written yet.

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

`screen.c` makes the screen and the parser and passes the parser's replies to the pty and its bell to drawing; the other files are stubs whose calls fail with ENOSYS.

## Design

### The pty, the screen and the loop

Task 16 of [milestone 66](../dev-log/66-terminal/plan.md) writes this section.

### Drawing

Task 17 writes this section.

### Keys, scrollback and selection

Task 22 writes this section.

## Building

`make term` builds `term` for the architecture `ARCH` selects, into `build/term/term` or `build/x86_64/term/term`, and the root `make` builds it too; `make term-host` builds it for the host, into `build/host/term/term`. [`mk/term.mk`](../mk/term.mk) has the rules, and [`sources.mk`](sources.mk) the one list of sources. The dialect is C99, as `libtsm.h`'s is. The Rach2 build compiles with the musl wrapper, links the architecture's `libtsm.a` before musl's `libc.a` and runs `elfcheck` on the program; the host build compiles with `HOSTCC` and `HOSTCFLAGS` and links the host's `libtsm.a`. Each object depends on the headers it includes and on its libtsm's stamp, which stages the patched `libtsm.h`.

### On Rach2

Tasks 20 and 27 write this section.

## Tests

### esctest2 and the Unicode tests

Task 24 writes this section.
