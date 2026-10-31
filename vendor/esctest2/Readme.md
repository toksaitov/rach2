# esctest2

esctest2 checks terminal escape sequences against the behavior its upstream tests specify. Rach2 runs its own driver on [`term -H`](../../term/Readme.md#term--h)'s host pty. It is a test product, with no file staged on a system image.

## Use

`make esctest2` builds the controller, `term-host` and the packaged Python driver. `tests/run esctest2` prepares and runs the case of [`host.esctest2.json`](../../tests/cases/host.esctest2.json). It needs a POSIX host with Python 3. The controller executes the upstream driver directly, without a shell or another Python driver. The collection records the interpreter, driver archive, terminal, controller and policy files as prepared inputs.

The milestone's end run includes `esctest2` with `style` and `host`. Its first failures join the end run's fixes; no results or accepted failures have been recorded yet. The release runs the registered suite too. Windows currently has no implementation of the POSIX pty used by `term -H`; the controller refuses it explicitly.

## Results

The controller requires a nonempty result file, one result per test name, a final count and a clean terminal exit. A failure retains its scratch directory and names it on stderr. `driver.log` has the upstream tracebacks, `results.tsv` the exact names and statuses, and `screen.sock` is removed after the children end. Upstream's driver can exit zero after failed tests, so its process status alone is insufficient.

[`known-failures.txt`](known-failures.txt) is empty until the end run has made and fixed its findings. Each accepted entry then has the form `ClassTests.test_Name|concrete reason`, sorted by name. Names are exact, with no pattern characters, duplicates or empty reasons. After that first list, changes may only remove entries. Every entry must occur once and fail in each run: a passing, unused or renamed entry fails the suite. An upstream skip or upstream known bug requires its own entry too.

[`protected-prefixes.txt`](protected-prefixes.txt) prevents exceptions for the upstream cases of the terminal features promised by tasks 18, 19 and 21. The keyboard, Unicode, synchronized-output, focus and clipboard host tests have no exception mechanism. Other upstream cases exercise features beyond those promises; an accepted failure must say which operation is absent or differs. The list is established by review in the end run, never generated from failing output.

## Sources and license

The tarball is GitHub's archive of [ThomasDickey/esctest2](https://github.com/ThomasDickey/esctest2) commit `2798f12149a19c3295e9b4853ab2da4b2eff1b2b`. [`mk/esctest2.mk`](../../mk/esctest2.mk) pins its full commit and SHA-256, `04b01a8343045a6801ea9a2a849b339e1b41179197102363a43fab13b25a2e1e`. Its `LICENSE` contains GPL version 2; individual files retain their upstream notices. None is linked into `term` or libtsm.

## Local changes

[`0001-headless-measurements.patch`](patches/0001-headless-measurements.patch) adds two optional arguments to the upstream driver. `--result-file` writes tab-separated `PASS`, `FAIL` or `SKIP` and the full method name after each test, followed by `END` and the count. `--snapshot-socket` connects to the controller before terminal setup and reports the driver's process ID for cleanup.

With the socket, screen-size and screen-character inspection use `term -H`'s `dump` and `cell` commands. A real CPR round trip first drains all preceding terminal output, so the snapshot observes that output without a sleep. This permits tests of screen operations even though libtsm lacks DECRQCRA. Direct protocol queries still use the terminal pty and receive no fabricated reply. DA1 checks term's declared VT220 with ANSI color, `62;22`, rather than xterm's compile-time options. A startup marker stays on the screen until the controller acknowledges it; a driver that exits before reaching it makes `wait` answer `missing`. The test methods otherwise keep their upstream expectations, decorators and report-read timeout; the C controller adds no time limit.

## Building

The Make rule verifies the tarball digest, extracts below `build/vendor/esctest2/source`, applies the patch with fuzz zero, compiles the Python files and packages them as `build/tests/host/esctest2-driver.zip`. The upstream `esctest.py` is also the archive's `__main__.py`; no wrapper script is added. Bytecode caches stay under `build/cache`. The interpreter is a link to the host's Python, which keeps its installed standard library available. The C controller and the host terminal build with the normal host flags. The prepared terminal, interpreter and policy files have `esctest2-` names under `build/tests/host/`, where collection preparation recognizes them as native products on the execution host.

## Updating

Replace the pinned tarball, commit and digest together, remake the patch on the new source, and compile `make esctest2`. Review changes to the selected upstream inventory and the protected prefixes. An exception whose name disappeared must be removed, and one for a new case cannot be added after the initial end-run list.
