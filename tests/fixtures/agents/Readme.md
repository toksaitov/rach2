# Coding agents' output

Recordings of what Claude Code writes to its terminal, for checking the console's glyphs and widths and `term`'s screen against a real agent's interface without running the agent on Rach2. Each `*.typescript` is a typescript of util-linux's `script`: a header line, the bytes the program wrote to an 80x25 terminal, and a trailer line.

## Use

Play a recording on the console with `cat`, at 80 columns:

    stty -echo; cat claude-rach2.typescript; stty echo

`claude-rach2.typescript` was recorded under `TERM=rach2`, the console's own, and `claude-xterm-256color.typescript` under `TERM=xterm-256color`, which `term` gives. Each has Claude Code 2.1.316's start screen with its logo in quadrant blocks, a prompt, the spinner of stars, a reply of a check mark, a rocket emoji and two CJK characters, the menu of commands, the model menu, and the screen it leaves at its exit. On the console, the reply's rocket and CJK characters are `?` and a blank, since the font has no glyph for them; the interface's own glyphs all have a stand-in. The console has no alternate screen, so what Claude Code drew there stays under the lines it writes at its exit. The console answers the queries in the recordings, and a shell's terminal echoes the answers onto the screen, which moves the cursor under Claude Code's relative moves, so the command above turns the echo off first, as Claude Code does.

## Updating

`sh tests/fixtures/agents/record.sh` records both again over the files here. It needs `claude` signed in, `script` and `tic`, and types its keys on a timer, so read each recording before committing it. Codex is signed in only in the owner's accounts and Gemini CLI was not installed where these were made, so their output is looked at on the Lenovo instead.
