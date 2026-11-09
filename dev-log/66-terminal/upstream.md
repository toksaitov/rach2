# Upstream faults of milestone 66

Faults this milestone found in another project's code, one section each, in the format of [`dev-log/upstream.md`](../upstream.md), which indexes them. The libtsm faults were read in the pinned tarball, `vendor/libtsm/libtsm-4.8.0.tar.gz`, SHA-256 `50811e9e94fc798eb5dcbaff20f81067c942ec47f329f91bbe4caeb35a61ff05`; their lines are in `src/tsm/`.

## Debian's chromium-headless-shell launcher does not exec

* Project: Debian 13's `chromium-headless-shell` package, as snapshot.debian.org had it at `20260927T082144Z` (`vendor/gnu/chromium/fetch_chromium.sh`); the package version is missing.
* Fails: `/usr/bin/chromium-headless-shell` is a `#!/bin/sh -e` script that runs `/usr/lib/chromium/chromium-headless-shell "$@"` without `exec`, so SIGTERM to the PID that Puppeteer, Playwright or a shell started ends only the shell, and the browser goes on with its debugging port.
* Found: 2026-11-01, by the agents' survey of Rach2 in milestone 66 (R2-016), in the GNU root.
* Reproduction: start the script, send SIGTERM to its PID, and list the processes; `dev-log/66-terminal/agents/claude-code/rach2-repro/chromium_orphans.sh` does it.
* Evidence: `dev-log/66-terminal/agents/claude-code/rach2-agent-env-report.md`, R2-016; commit `1b2540369`.
* Rach2: `vendor/gnu/install_root.sh` replaces the script with a link to the browser ([`vendor/gnu/chromium/Readme.md`](../../vendor/gnu/chromium/Readme.md), Local changes). The second half of R2-016, the GPU and network children outliving the browser, is not this fault and its cause is open.
* Status: unreported.
* Next: check the current Debian package's script and Debian's bug tracker for it; the report goes to Debian's chromium package.

## libtsm sends Shift F1 as F11 and swaps the keypad's divide and multiply

* Project: libtsm 4.8.0.
* Fails: the key encoder sends Shift+F1 as F11's `CSI 23 ~` (`tsm-vte.c:3255-3261`), and in application keypad mode sends `SS3 j` for divide and `SS3 o` for multiply, where xterm sends `SS3 o` and `SS3 j` (`tsm-vte.c:3213-3224`).
* Found: 2026-11-01, in milestone 66's reading of libtsm before `term`'s key work.
* Reproduction: call `tsm_vte_handle_keyboard` with `XKB_KEY_F1` and the Shift mask, and with `XKB_KEY_KP_Divide` in application keypad mode, and compare the bytes with xterm's.
* Evidence: `dev-log/66-terminal/plan.md`, the libtsm findings; `vendor/libtsm/patches/0018-xterm-keys.patch`.
* Rach2: the patch sends what xterm sends with its default resources, these keys among the rest.
* Status: unreported.
* Next: check libtsm's current `tsm-vte.c`; send the two fixes apart from the patch's wider changes.

## libtsm takes CSI with a greater-than marker for the plain sequence

* Project: libtsm 4.8.0.
* Fails: `CSI > p`, xterm's pointer mode, soft-resets the terminal (`tsm-vte.c:2030-2033`), and `CSI > q`, XTVERSION, sets the LEDs as DECLL, since `case 'q'` does not look at the marker (`tsm-vte.c:2151-2165`).
* Found: 2026-11-01, in milestone 66, while `term` answered the probes programs send.
* Reproduction: write `ESC [ > 1 p` after setting an attribute and see the attribute reset.
* Evidence: `vendor/libtsm/patches/0021-probes-modes-strings.patch`.
* Rach2: the patch ignores `CSI > p` and answers XTVERSION.
* Status: unreported.
* Next: check current libtsm.

## libtsm's CSI collector drops the less-than and equals markers

* Project: libtsm 4.8.0.
* Fails: the CSI collector drops `<` and `=` (`tsm-vte.c:1019-1056`), so a sequence that carries one is taken for the plain sequence with the same final byte.
* Found: 2026-11-01, in milestone 66's reading of libtsm for the Kitty keyboard protocol's `CSI < u` and `CSI = flags u`.
* Reproduction: write `ESC [ = c` and see the answer to a plain `CSI c`.
* Evidence: `dev-log/66-terminal/plan.md`; `vendor/libtsm/patches/0018-csi-markers-scosc-scorc.patch`.
* Rach2: the patch collects them as `CSI_LT` and `CSI_EQUAL` and ignores a sequence that carries either unless its final byte is `u`.
* Status: unreported.
* Next: check current libtsm.

## libtsm's combined symbols skip an index and free an interior pointer

* Project: libtsm 4.8.0.
* Fails: `tsm_symbol_table_new` starts `next_id` at `TSM_UCS4_MAX + 2`, so the first combined symbol misses index entry 1 (`tsm-unicode.c:158`); and on an insert or index growth failure `tsm_symbol_append` frees `nval`, which points one word past its allocation's start (`tsm-unicode.c:310`).
* Found: 2026-11-01, in milestone 66, while `term` kept combining marks.
* Reproduction: append a combining mark to a base character and look up the symbol; the free needs an allocation failure, for which a failing `malloc` hook will do.
* Evidence: `vendor/libtsm/patches/0019-symbol-indices.patch`.
* Rach2: the patch starts `next_id` at `TSM_UCS4_MAX + 1` and frees `nval - 1`, as `free_ucs4` does.
* Status: unreported.
* Next: check current libtsm; the wrong free is the one to report first.

## libtsm clips a wide character in the last column and drops combining marks

* Project: libtsm 4.8.0.
* Fails: with autowrap on, `tsm_screen_write` clips a character of width 2 that starts in the last column instead of wrapping it, and drops a character of width 0, so combining marks are lost (`tsm-screen.c:1099-1132`).
* Found: 2026-11-01, in milestone 66's reading of libtsm.
* Reproduction: write a CJK character with the cursor in the last column, and `e` followed by U+0301.
* Evidence: `dev-log/66-terminal/plan.md`; `vendor/libtsm/patches/0019-wide-and-combining.patch`.
* Rach2: the patch wraps before a wide character that does not fit and appends a zero-width code point to the preceding cell with `tsm_symbol_append`.
* Status: unreported.
* Next: check current libtsm; the dropped marks may be judged a missing feature, the clipping is the fault.

## libtsm's DECSTBM leaves the cursor in place

* Project: libtsm 4.8.0.
* Fails: DECSTBM sets the margins and does not move the cursor to the home position, as the VT100 and xterm do (`tsm-screen.c:740-765`, `tsm-vte.c:2057-2066`).
* Found: 2026-11-01, in milestone 66's esctest2 run on `term`'s host pty; `DECSTBMTests.test_DECSTBM_MovsCursorToOrigin` fails.
* Reproduction: move the cursor, send `ESC [ 2 ; 5 r`, and read the cursor position.
* Evidence: `vendor/esctest2/known-failures.txt`.
* Rach2: nothing; the case is listed as a known failure.
* Status: unreported.
* Next: check current libtsm.

## libtsm's REP without a parameter fills the rest of the line

* Project: libtsm 4.8.0.
* Fails: `CSI b` with no parameter passes the default -1 as an unsigned count, so the last character is repeated to the end of the line instead of once (`tsm-vte.c:1012-1013, 2147-2149`, `tsm-screen.c:1487-1515`).
* Found: 2026-11-01, in milestone 66's esctest2 run on `term`'s host pty; `REPTests.test_REP_DefaultParam` fails.
* Reproduction: write `a` then `ESC [ b`.
* Evidence: `vendor/esctest2/known-failures.txt`.
* Rach2: nothing; the case is listed as a known failure.
* Status: unreported.
* Next: check current libtsm.

## libtsm's REP does not wrap

* Project: libtsm 4.8.0.
* Fails: `tsm_screen_repeat_char` stops at the right edge, where writing the characters would wrap (`tsm-screen.c:1487-1515`).
* Found: 2026-11-01, in milestone 66's esctest2 run on `term`'s host pty; `REPTests.test_REP_RespectsTopBottomMargins` fails.
* Reproduction: write a character two columns from the right edge, then `ESC [ 5 b`.
* Evidence: `vendor/esctest2/known-failures.txt`.
* Rach2: nothing; the case is listed as a known failure.
* Status: unreported.
* Next: check current libtsm.

## libtsm scrolls the region on a line feed below it

* Project: libtsm 4.8.0.
* Fails: a line feed on the last line of the screen, below the scrolling region, scrolls the region, where xterm leaves it: `tsm_screen_move_down` calls `screen_scroll_up`, which scrolls between the margins (`tsm-screen.c:1224-1246, 280-292`).
* Found: 2026-11-01, in milestone 66's esctest2 run on `term`'s host pty; `DECSTBMTests.test_DECSTBM_CursorBelowRegionAtBottomTriesToScroll` fails.
* Reproduction: set `ESC [ 2 ; 5 r`, move to the last line, and send a line feed.
* Evidence: `vendor/esctest2/known-failures.txt`.
* Rach2: nothing; the case is listed as a known failure.
* Status: unreported.
* Next: check current libtsm.

## libtsm's RIS leaves the alternate screen's text

* Project: libtsm 4.8.0.
* Fails: RIS switches to the main screen and erases only it, so the alternate screen's text is there again the next time it is entered without erasing (`tsm-vte.c:885-894`, `tsm-screen.c:951-975`).
* Found: 2026-11-01, in milestone 66's esctest2 run on `term`'s host pty; `RISTests.test_RIS_ExitAltScreen` fails.
* Reproduction: enter mode 1047, write text, send `ESC c`, and enter mode 1047 again.
* Evidence: `vendor/esctest2/known-failures.txt`.
* Rach2: nothing; the case is listed as a known failure.
* Status: unreported.
* Next: check current libtsm.
