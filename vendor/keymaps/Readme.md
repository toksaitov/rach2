# Keymaps

This directory has the keyboard layouts of Rach2's console, as kbd's text keymaps in [`xkb/`](xkb): the layouts of the CIS countries, Kyrgyz first, and the common Western ones, and small add-on maps that pick the key which switches between a map's two layouts or change Caps Lock. The maps are made from XKB's data, xkeyboard-config, by console-setup's `ckbcomp` in the pinned Ubuntu container, checked there, and committed with their provenance in [`manifest.txt`](manifest.txt). A build stages the committed files and runs no container; only [`regen_keymaps.sh`](regen_keymaps.sh) makes them, by the procedure under Updating.

## Use

The images that stage kbd's programs, those with Dropbear and the GNU root, stage `xkb/` as `/usr/share/kbd/keymaps/xkb`, where kbd's `loadkeys` finds a map by its name. The recovery root stages only `us.map` and its two includes there, the map its init loads, since its 5 MiB must keep 512 KiB free. A layout is loaded first and add-ons after it:

```
loadkeys -u kg alt-shift
loadkeys -u de caps-ctrl
```

`/etc/vconsole.conf` names the layout and the add-ons that init loads at boot ([`vconsole.conf(5)`](../../man/man5/vconsole.conf.5)).

## Interfaces

The layouts, each a file `NAME.map`:

| Map | XKB layouts | Language |
|---|---|---|
| `kg` | `us,kg` | Kyrgyz |
| `kz` | `us,kz` | Kazakh |
| `ru` | `us,ru` | Russian |
| `ua` | `us,ua` | Ukrainian |
| `by` | `us,by` | Belarusian |
| `uz` | `us,uz` | Uzbek, Cyrillic |
| `uz-latin` | `us,uz(latin)` | Uzbek, Latin |
| `tj` | `us,tj` | Tajik |
| `ru-tt` | `us,ru(tt)` | Tatar |
| `ru-bak` | `us,ru(bak)` | Bashkir |
| `az` | `us,az` | Azerbaijani |
| `tm` | `us,tm` | Turkmen |
| `md` | `us,md` | Romanian of Moldova |
| `am` | `us,am` | Armenian |
| `ge` | `us,ge` | Georgian |
| `us` | `us` | English, US |
| `gb` | `gb` | English, British |
| `de` | `de` | German |
| `fr` | `fr` | French |
| `us-intl` | `us(intl)` | US International, with dead keys |
| `us-dvorak` | `us(dvorak)` | Dvorak |
| `us-colemak` | `us(colemak)` | Colemak |

A CIS map has two layouts: US first, which the console starts in, and the country's second, which a switch reaches, so that a Latin command line is one switch away. The Western maps have one. Each map uses Linux's tables 0 to 31, those of Shift, AltGr, Control, Alt and ShiftL: the second layout is on ShiftL, and the switch toggles `ShiftL_Lock`, as ckbcomp writes it. On the second layout, Ctrl and Alt give what they give on US, so Ctrl+C sends ETX and Alt+B sends ESC b whichever layout is on. A map of one layout has the same entries on ShiftL as without it, so a switch does nothing there.

The add-ons, loaded after a layout:

| Map | What it does |
|---|---|
| `alt-shift` | Left Alt and either Shift, in either order, switch layouts. The default. |
| `ctrl-shift` | Either Ctrl and either Shift, in either order, switch layouts. |
| `caps-toggle` | Caps Lock switches layouts; Shift+Caps Lock is Caps Lock. |
| `ralt-toggle` | Right Alt switches layouts, and is no longer AltGr. |
| `super-space` | Super, pressed first, and Space switch layouts. Super holds CtrlL, which no other key uses, so Super with any other key types nothing. |
| `caps-ctrl` | Caps Lock is Control. |
| `caps-escape` | Caps Lock is Escape. |

An add-on changes only the entries of its own keys. Two add-ons on the same key, such as `caps-toggle` and `caps-ctrl`, leave the last one loaded in force.

Every layout includes two files beside it. `common.inc`, which `regen_keymaps.sh` writes, has the US map's keys; a layout includes it first and then gives only the entries in which it differs. `xterm-strings.inc`, written by hand, gives the function keys xterm's sequences in place of Linux's console strings: `ESC O P` to `ESC O S` for F1 to F4, `ESC [ 15 ~` and on for F5 to F12, the same with the modifier parameters 2, 5 and 6 for F13 to F48, where ckbcomp puts Shift, Ctrl and Ctrl+Shift with F1 to F12, and `ESC [ H`, `ESC [ F`, `ESC [ 2 ~`, `ESC [ 3 ~`, `ESC [ 5 ~` and `ESC [ 6 ~` for Home, End, Insert, Delete, Page Up and Page Down. The maps give no accent table: the dead keys of `de`, `fr`, `gb`, `md`, `us-intl`, `us-dvorak` and `us-colemak` compose through the kernel's default accent table.

## Sources and license

The layouts are xkeyboard-config's, the Ubuntu package `xkb-data`, whose data is under the MIT license and MIT-style licenses; its `COPYING`, `/usr/share/doc/xkb-data/copyright` in the package, carries every notice. `ckbcomp`, from the Ubuntu package `keyboard-configuration` (console-setup), is a Perl program under the GNU General Public License, version 2 or later; it runs only in the container, as a tool, and none of its code is in a map. It reads the XKB files itself and turns X keysyms into Unicode with a table built into the program. It also appends console-setup's `compose.CHARMAP.inc`, only when given `-charmap`, which `regen_keymaps.sh` does not give, and the local file `/etc/console-setup/remap.inc`, which in the package holds only comments; `gen_keymaps.sh` requires the lines after the strings to be comments and drops them. `manifest.txt` records the container, the archive snapshot, each package's version and `.deb` digest, every package the install added, the layouts, and each file's size and SHA-256.

## Local changes

`gen_keymaps.sh` changes what ckbcomp writes in four ways:

* It keeps tables 0 to 31. ckbcomp writes 128: tables 32 to 63 for a third and fourth layout on ShiftR, which no map here has, and 64 to 127 with capitals for Caps Lock under `CtrlL_Lock`, since Linux's Caps Lock does not shift a Unicode letter. Rach2's does, so Caps Lock is `Caps_Lock` again and its LED follows it.
* Ctrl and Alt on the second layout take the entries of the first: ckbcomp gives them the second layout's letters, so Ctrl+C on a Cyrillic layout would type a letter.
* `strings as usual`, Linux's console strings, becomes `include "xterm-strings"`.
* The rows of the US map move into `common.inc`, which shrinks the maps from about 740 KB to about 185 KB.

## Design

* A map is checked against XKB through libxkbcommon, a second reading of the same data: `xkbcli compile-keymap` gives each key's symbols by group and level, and `check_keymap.awk` requires every character of the main block's keys on levels 1 to 4 to be the one in the map's table for its group and level, by `loadkeys -u -m`, with the character of each keysym from X11's `keysymdef.h`.
* An add-on is checked on every layout: the tables with it loaded may differ from the layout's only in its keys, and a replay of presses and releases with Linux's rules for `KT_SHIFT` and `KT_LOCK` must switch the layout from either one, with the two keys pressed in either order and released in either order, and leave no modifier held.
* `loadkeys -u -m` must read each map with its includes into the same tables as ckbcomp's rows after the changes above, and must find the strings.
* The container's `loadkeys` is kbd 2.6.4, Ubuntu's; Rach2 stages kbd 2.10.0. Both find an include beside the file that includes it and know the keysym names the maps use, `dead_kbreve` and its kind among them, and 2.10.0 searches its data directory's `keymaps` and every directory under it for a map named on the command line.

## Updating

The maps change only through `regen_keymaps.sh`, a maintainer procedure that needs Docker:

```
sh vendor/keymaps/regen_keymaps.sh
```

The script runs the container whose digest [`tools/image/ubuntu-image.txt`](../../tools/image/ubuntu-image.txt) has, for `linux/amd64`, installs `ca-certificates` from the container's default archive, points apt at one dated state of the Ubuntu snapshot archive, `SNAPSHOT` in the script or `RACH2_KEYMAPS_SNAPSHOT` in the environment, and installs `keyboard-configuration`, `xkb-data`, `kbd`, `libxkbcommon-tools` and `x11proto-dev` from it. [`gen_keymaps.sh`](gen_keymaps.sh) makes and checks every map there, as Design says, and stops on the first failure. The script makes everything twice, in `build/keymaps-regen/round1` and `round2`, requires the two rounds to be equal byte for byte, and copies the maps, `common.inc` and the manifest into this directory. The layouts and the add-ons are lists in `gen_keymaps.sh`; a new add-on needs a check in `check_keymap.awk` too.

## Limits

* The kernel console's font has the letters of Russian, Kyrgyz, Kazakh, Ukrainian and Belarusian, and draws the others as `?`: the letters of the Latin-1 range, so German, French and the Latin letters of US International; the Azerbaijani, Turkmen and Romanian letters; the extra Cyrillic letters of Uzbek, Tajik, Tatar and Bashkir, U+0496 to U+04EF among them; the Armenian and Georgian scripts; and the modifier letter U+02BB of Uzbek Latin. `term` draws them all, with Unifont.
* ckbcomp knows no X keysym for a few dead keys, `dead_belowmacron` among them, and leaves those entries empty.
* `ralt-toggle` takes AltGr from the layouts that have it: `az`, `de`, `fr`, `gb`, `ge`, `md`, `ru-bak`, `ru-tt`, `tj`, `tm`, `ua`, `us-intl` and `us-colemak`.

## Tests

The checks under Design run in the container on every regeneration, and nothing is committed unless all pass. The keyboard's machine cases load `kg` with `alt-shift` and press keys on it.
