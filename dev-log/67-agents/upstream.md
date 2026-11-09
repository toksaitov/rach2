# Upstream faults of milestone 67

Faults this milestone found in another project's code, one section each, in the format of [`dev-log/upstream.md`](../upstream.md), which indexes them.

## libtsm's mode 1049 keeps the alternate screen's attributes

* Project: libtsm 4.8.0, `vendor/libtsm/libtsm-4.8.0.tar.gz`, SHA-256 `50811e9e94fc798eb5dcbaff20f81067c942ec47f329f91bbe4caeb35a61ff05`.
* Fails: modes 1049 and 1048 save and restore only the cursor's position (`src/tsm/tsm-vte.c:1764-1795`), and leaving the alternate screen does not put back the main screen's erase attributes, which `tsm-screen.c` stores on entry (`src/tsm/tsm-screen.c:997, 1011-1030`). After vim leaves with its color scheme's SGR in effect, a clear paints the main screen in the scheme's background.
* Found: 2026-11-01, in milestone 67's task 21a, from the owner's case in `term`: vim, `:colorscheme blue`, quit, Ctrl+L.
* Reproduction: enter mode 1049, set a background color, leave the mode, and clear the screen; the colour case of task 21a writes these bytes with printf and reads the grid's colours from the screen dump.
* Evidence: `dev-log/67-agents/plan.md`, 21a; `vendor/libtsm/patches/0067-alternate-screen-cursor.patch`; commit `25987d576`.
* Rach2: the patch saves and restores the cursor as DECSC and DECRC do, in the main screen's slot, as xterm does, and restores the main screen's erase attributes.
* Status: unreported.
* Next: check libtsm's current `tsm-vte.c` and `tsm-screen.c`; the patch rests on 0024's saved cursor for each screen and would need to stand alone for upstream.
