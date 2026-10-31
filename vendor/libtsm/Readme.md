# libtsm

libtsm is the terminal emulator's state machine that [`term`](../../term/Readme.md) is built on: it parses what a program writes to its terminal, VT100 to xterm's sequences, into a screen of cells with their attributes and a scrollback, and turns keys into the bytes a program expects. It has no drawing or input of its own; `term` draws its cells on the framebuffer and gives it the keys. It is built here as a static library, `libtsm.a`, with its header `libtsm.h`, for each architecture against the ported [musl](../musl/Readme.md), and for the host, where `term -H` and the tests that drive it run.

## Sources and license

The source is GitHub's archive of the tag `v4.8.0` of [github.com/kmscon/libtsm](https://github.com/kmscon/libtsm), `libtsm-4.8.0.tar.gz` in this directory. [`build_libtsm.sh`](build_libtsm.sh) has its version in `VERSION` and its SHA-256 in `TARBALL_SHA256`, and refuses a tarball whose checksum differs; the root `Makefile` gives the same file as `LIBTSM_TARBALL`.

libtsm is under the MIT license, which its `COPYING` gives with the names of its authors. The library compiles one file from outside its own sources, `external/wcwidth/wcwidth.c`, Fredrik Fornwall's port of Jeff Quast's wcwidth, under the MIT license, with parts from Markus Kuhn's wcwidth under a permissive notice; `external/wcwidth/LICENSE.txt` has both. The tarball also has a hash table, `src/shared/shl-htable.c` and `shl-htable.h`, under the LGPL 2.1 or later, with code inline in the header; neither is compiled, as Local changes says. The tarball's other files under `src/shared/` are MIT or public domain, and `external/xkbcommon/xkbcommon-keysyms.h`, the key names, is MIT.

The Kitty keyboard protocol's key encoder, which [`0018-kitty-keyboard-protocol.patch`](patches/0018-kitty-keyboard-protocol.patch) adds to `tsm-vte.c`, is ported from `kitty_kbd_protocol()` in `input.c` and the table of `kitty-keymap.h` of [foot](https://codeberg.org/dnkl/foot), under foot's MIT license; the patch puts foot's copyright notice and license above the code.

## Local changes

The hash table. `build_libtsm.sh` removes `shl-htable.c`, `shl-htable.h` and `LICENSE_htable` from the extracted tree and copies [`shl-htable.c`](shl-htable.c) and [`shl-htable.h`](shl-htable.h) from this directory in their place: a table of Rach2's own, with the part of the interface libtsm uses, `shl_htable_init`, `_clear`, `_visit`, `_lookup`, `_insert` and `_remove`, by separate chaining over buckets that double when the table holds more elements than buckets. Only `tsm-unicode.c` uses it, for the symbols of several code points, a character and its combining marks. The script then stops if any file of the tree still carries the LGPL's notice.

`wcwidth`. `wcwidth.c` defines `wcwidth()`, as musl's `libc.a` does; every file is compiled with `-Dwcwidth=tsm_wcwidth`, so that a program linked with both keeps musl's and libtsm keeps its own, as libtsm's version script keeps the symbol private in a shared build.

The patches, which [`patches/series`](patches/series) names in the order they apply, each with `patch -p1 -F 0 --forward`. Each is made on the tree the ones before it leave, and a patch whose context has moved fails rather than applying in the wrong place; a patch in `patches/` that the series does not name stops the build. Each begins with a description and is written to be offered to kmscon/libtsm.

* [`0001-declare-sync-focus-clipboard.patch`](patches/0001-declare-sync-focus-clipboard.patch) declares, in `libtsm.h`, `tsm_vte_get_sync_hold()` and `tsm_vte_end_sync_hold()` for synchronized output, mode 2026; `tsm_vte_handle_focus()` for focus events, mode 1004; and `tsm_vte_set_clipboard_cb()` with its callback type for OSC 52. In `tsm-vte.c` they are stubs: the hold is never on, no focus report is sent, and the callback is kept but never called; `0021-probes-modes-strings.patch` implements them.
* [`0002-version-macros.patch`](patches/0002-version-macros.patch) defines `TSM_VERSION_MAJOR`, `TSM_VERSION_MINOR`, `TSM_VERSION_MICRO` and `TSM_VERSION`, `"4.8.0"`, in `libtsm.h`, which `term -V` prints. The script stops when the patched header's `TSM_VERSION` is not `VERSION`.
* [`0019-wide-and-combining.patch`](patches/0019-wide-and-combining.patch) wraps a wide character before writing it when autowrap is on and the remaining columns cannot hold it. A character wider than the screen, or one at the edge with autowrap off, keeps the clipping behavior. Zero-width Unicode code points join the preceding occupied cell in the same row through `tsm_symbol_append`, including a wide base or one awaiting wrap; the base keeps its width and attributes, its age changes, and the cursor stays put. Marks at column zero or after an empty cell, controls and invalid code points are ignored.
* [`0019-symbol-indices.patch`](patches/0019-symbol-indices.patch) fixes the combined symbols' IDs to match their index entries and frees the allocation from its prefix on an insertion or index growth failure. Both are needed by the screen's combining path.
* [`0018-csi-markers-scosc-scorc.patch`](patches/0018-csi-markers-scosc-scorc.patch) makes the CSI collector keep the private markers `<` and `=`, which it dropped, so that the Kitty keyboard protocol's `CSI < u` and `CSI = flags u` are not read as a plain `CSI u`; a sequence with either marker and a final byte other than `u` is ignored. It adds SCOSC, `CSI s`, and SCORC, `CSI u`, which save and restore the cursor as DECSC and DECRC do, taken only without a private marker and without parameters.
* [`0018-xterm-keys.patch`](patches/0018-xterm-keys.patch) makes the key encoder send what xterm sends, the bytes of [`tests/host/keytable.txt`](../../tests/host/keytable.txt) for each keysym: F1 to F12, the editing keys and the cursor keys carry xterm's modifier parameter for Shift, Alt and Control, so that Shift+F1 is `CSI 1 ; 2 P` and no longer F11's `CSI 23 ~`; the application keypad's divide is `SS3 o` and multiply `SS3 j`, which were swapped, and a keypad key with modifiers is `SS3 m final`; with Num Lock on, which the new `TSM_NUM_LOCK_MASK` reports, the keypad sends its text in application mode too, as xterm's numLock resource does; Control makes control characters as Xlib does. The patch's description lists the rest.
* [`0018-kitty-keyboard-protocol.patch`](patches/0018-kitty-keyboard-protocol.patch) adds the [Kitty keyboard protocol](https://sw.kovidgoyal.net/kitty/keyboard-protocol/) with its flags 1 (disambiguate), 2 (repeat and release events) and 4 (alternate keys): the query `CSI ? u`, the push `CSI > flags u`, the pop `CSI < n u` and the set `CSI = flags ; mode u`, with a stack of eight entries for each of the main and the alternate screen, emptied by a reset. A set or a push drops flags 8 and 16, so that a query reports what took effect. `tsm_vte_handle_key()` takes a key's press, repeat or release and, for flag 4, the character the key gives unshifted and the one it gives in the base layout; `tsm_vte_handle_keyboard()` is a press through it, and `tsm_vte_get_key_flags()` returns the flags in effect. The encoder is ported from foot's, as Sources and license says.
* [`0021-probes-modes-strings.patch`](patches/0021-probes-modes-strings.patch) fills in the calls of `0001-`: `CSI ? 2026 h` holds the screen's updates until `CSI ? 2026 l`, a reset or `tsm_vte_end_sync_hold()`, and `tsm_vte_handle_focus()` sends `CSI I` or `CSI O` while mode 1004 is set. It answers the probes an application sends to learn its terminal, each as it comes, so that the answers keep the probes' order and one before DA1's shows that its probe is supported: DA1 as a VT220 with ANSI color, `CSI ? 62 ; 22 c`; CPR, `CSI 6 n`, and DECXCPR, `CSI ? 6 n`, counting the row from the top margin in origin mode; XTVERSION, `CSI > q` and `CSI > 0 q`, with `DCS > | libtsm(4.8.0) ST`; DECRQM, `CSI Ps $ p` and `CSI ? Ps $ p`, for every mode the parser takes, those it ignores as fixed and mode 2027 as not recognized; OSC 10 and 11 for the exact queries. DECLL takes only `CSI Ps q`, and `CSI > p`, xterm's pointer mode, is ignored where it reset the terminal. The OSC buffer grows from 128 bytes as a string needs, up to 2 MiB, which holds OSC 52 with 1 MiB of text in base64, and is freed after a long string; OSC 52 goes to the clipboard callback, or to the OSC callback when none is set. A longer string is consumed and dropped, and one that CAN, SUB or a C1 control other than ST ends is cancelled.

## Building

`make libtsm` builds the library for the architecture `ARCH` selects, i686 by default, and `make libtsm-host` for the host; `make term` and `make term-host` build them as they need them, and the root `make` builds the architecture's through `term`. Each runs `build_libtsm.sh` through `etude build`, behind a stamp of its own:

| Build | Extracted tree and stamp | Stage |
| --- | --- | --- |
| i686 | `vendor/libtsm/` | `build/vendor/libtsm/` |
| x86-64 | `build/x86_64/vendor-work/libtsm/` | `build/x86_64/vendor/libtsm/` |
| host | `build/host/vendor-work/libtsm/` | `build/host/vendor/libtsm/` |

Each stage has `lib/libtsm.a` and `include/libtsm.h`, the patched header; an architecture's has `libtsm.tar.bz2` too, the patched sources that [`term`'s guest build](../../term/Readme.md#on-rach2) compiles on Rach2. `build_libtsm.sh`, run as `RACH2_ARCH=ARCH ./build_libtsm.sh` or `./build_libtsm.sh --host`:

1. Checks the tarball's SHA-256, deletes the extracted tree and its objects and extracts it again.
2. Puts Rach2's hash table in place of libtsm's and stops if a file still carries the LGPL's notice.
3. Applies the series, and stops on a patch that is missing, does not apply or is not named, and on a `TSM_VERSION` that is not `VERSION`.
4. Compiles the files [`sources.mk`](sources.mk) lists, the six of `src/tsm/`, the hash table and `wcwidth.c`, one by one, with Meson's flags for the project less its `-Werror`, and `-Dwcwidth=tsm_wcwidth`: through the `muslgcc` wrapper for Rach2, and with `HOSTCC`, `cc` unless set, for the host. libtsm's `config.h` is not made: its one setting, `BUILD_ENABLE_DEBUG`, is off when undefined.
5. Puts the objects in `libtsm.a` with the architecture's `ar` and copies `libtsm.h` beside it.
6. For an architecture, packs the files compiled, every header of `src/tsm/`, `src/shared/`, `external/wcwidth/` and `external/xkbcommon/`, `COPYING` and `external/wcwidth/LICENSE.txt` in name order as `libtsm.tar.bz2`, some 90 KiB, which the images that stage the kernel's sources stage as `/src/term/vendor/libtsm.tar.bz2`, with `sources.mk` as `/src/term/vendor/libtsm.mk`.

`sources.mk` is the one list of the files compiled, each under `$(LIBTSM)`, the top of the extracted tree, which the includer sets; the script reads it without the prefix.

The stamp of either depends on the script, `sources.mk`, the tarball, the series, each patch and the hash table; the architecture's also on `muslgcc`, `mk/arch.sh` and musl's stamp. The architecture's step is keyed by its cross compiler, the host's by the host compiler.

## Updating

1. Put the new tarball in this directory and delete the old one.
2. Change `VERSION` and `TARBALL_SHA256` in `build_libtsm.sh`, and `LIBTSM_TARBALL` in the root `Makefile`.
3. Remake each patch on the new tree in the series' order, and the version in `0002-version-macros.patch`; a patch that applies only with fuzz is refused.
4. Check that `tsm-unicode.c` uses no call of the hash table that `shl-htable.h` here lacks, and that the new tree carries the LGPL nowhere else.
5. Rebuild `term` for both architectures and the host.

## Limits

The Kitty keyboard protocol has flags 1, 2 and 4 and not 8 (report all keys as escape codes) or 16 (report associated text): a set or a push drops them, and a query does not report them. Modifier keys are therefore never reported. The protocol's hyper and meta modifiers have no `TSM_*_MASK` and are not sent.

The parser answers no DCS request, such as DECRQSS or XTGETTCAP, and no OSC query but OSC 4, 10 and 11; OSC 52's query, `?`, goes to the clipboard callback, which decides. An OSC string longer than 2 MiB is dropped whole. Modes 2027, grapheme clustering, and 12, the blinking cursor, are not implemented, and DECRQM reports them as not recognized.

## Tests

`tests/host/libtsm_width_hosttest.c`, listed as `libtsm_width` in the host suite, compiles the patched sources with the sanitizers. It checks wide wrapping, scroll regions, scrollback, combining marks and cursor positions through the public draw and VTE interfaces, and injects allocation failures while creating combined symbols. The host-test Readme describes the cases.

[`tests/host/libtsm_keys_hosttest.c`](../../tests/host/libtsm_keys_hosttest.c) compiles the patched library with the host test and checks the three `0018-` patches: the protocol's requests and their stacks, SCOSC and SCORC, xterm's bytes for the function, editing, cursor, keypad and control keys, and the protocol's keys, among them its specification's own examples. [`tests/host/Readme.md`](../../tests/host/Readme.md) lists its rows.

[`tests/host/libtsm_probes_hosttest.c`](../../tests/host/libtsm_probes_hosttest.c) compiles the patched library with the host test and checks `0021-probes-modes-strings.patch`: modes 2026 and 1004 through their calls, the answers to the probes and their order, DECRQM for each mode, OSC 52, and unknown and oversized strings of each kind, whole and split across reads.
