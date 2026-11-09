# Upstream faults of milestone 27

Faults this milestone found in another project's code, one section each, in the format of [`dev-log/upstream.md`](../upstream.md), which indexes them.

## termbox2 misspells its Windows unsupported message

* Project: termbox2 2.7.0-dev, as `krffs/vendor/termbox2.h` gives `TB_VERSION_STR`; the upstream commit is missing.
* Fails: `tb_strerror` returns "Unsupporrted (Windows)" for `TB_ERR_WIN_UNSUPPORTED`. The generator `krffs/vendor/windows.sh:40` writes the string into `krffs/vendor/termbox2_win.h:2823`.
* Found: by 2026-08-21, in milestone 27's prose review, which read the vendored files and left them unchanged.
* Reproduction: run `krffs/vendor/windows.sh` on `termbox2.h`, or read `termbox2_win.h:2823`.
* Evidence: `dev-log/27-prose/Readme.md`, "Vendored upstream, read but not edited"; commit `fec567065`.
* Rach2: nothing; the vendored files stay as they came.
* Status: unreported.
* Next: find where `windows.sh` comes from, which milestone 27's record calls upstream termbox2 without naming a repository or commit, and check that source's current text.
