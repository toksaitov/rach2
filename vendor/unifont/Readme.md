# GNU Unifont

GNU Unifont is the bitmap font `term` draws with: a glyph 16 pixels high for nearly every character Unicode assigns, 8 pixels wide for most scripts and 16 for the wide ones, CJK among them. This directory has its release as GNU publishes it, the text `unifont_all-18.0.01.hex.gz`, and the build packs that text into one file `term` reads. The kernel console's font, `rach2/sys/display/vga_font.c`, comes from the same file through [`tools/font/mkfont.sh`](../../tools/font/mkfont.sh).

## Interfaces

[`term/unifont.h`](../../term/unifont.h) defines the packed font: a header, runs of consecutive code points of one width, and the glyphs' rows as the text has them, every number little-endian. [`term/unifont.c`](../../term/unifont.c) checks a file against that format with `unifont_open` and finds a glyph with `unifont_glyph`, a binary search over the runs; `term` reads the font through them. The packed font of 18.0.01 is 3,762,580 bytes in 3,735 runs.

## Sources and license

`unifont_all-18.0.01.hex.gz` and its signature `unifont_all-18.0.01.hex.gz.sig` are from [ftp.gnu.org/gnu/unifont/unifont-18.0.01/](https://ftp.gnu.org/gnu/unifont/unifont-18.0.01/). `gpgv` with the GNU keyring ([ftp.gnu.org/gnu/gnu-keyring.gpg](https://ftp.gnu.org/gnu/gnu-keyring.gpg)) verified the signature as made by Paul Hardy's key `95D2E9AB8740D8046387FD151A09227B1F435A33`. Its SHA-256 is `14c96e497466a82e46cf20032ce510e186bdc8658c9158e2290f452cda9bc498`, which `UNIFONT_SHA256` in [`mk/unifont.mk`](../../mk/unifont.mk) and `HEX_SHA256` in `mkfont.sh` hold, and each refuses a file with another digest.

The text has 126,743 glyphs, one a line: 60,518 of plane 0, 49,903 of plane 1, 6,586 of plane 2 and 1,110 of plane 3, the last two CJK ideographs, 368 tags and variation selectors of plane 14, and 8,258 letters of plane 15 that the ConScript Unicode Registry assigns in private use, as it does some of plane 0's. 21,127 of them are 8 pixels wide and 105,616 are 16.

Unifont is under two licenses, of which a user may take either, as [unifoundry.com](https://unifoundry.com/unifont/) states: the GNU General Public License, version 2 or later, with the GNU Font Embedding Exception, and the SIL Open Font License 1.1. Rach2 takes it under the SIL Open Font License, as the header of `vga_font.c` says. The packed font is made from it and is under the same terms.

## Building

`make unifont` checks the text's SHA-256, unpacks it into `build/unifont/` and runs [`tools/font/mkunifont.c`](../../tools/font/mkunifont.c), built as `build/tools/font/mkunifont`, which writes `build/unifont/unifont.font`; the stamp `build/unifont/.stamp` records that the step ran. The font is the same for i686 and x86-64 and is built once for both. The step needs `gzip` on the build host.

`mkunifont HEX OUT` refuses a line that is not a code point of four to six hexadecimal digits, a colon and 32 or 64 hexadecimal digits, a code point above U+10FFFF, and one not above the line's before it, and names the line. It packs every glyph, splitting a run where the code points stop being consecutive, where the width changes or at 65,535 glyphs. It then opens what it packed with `unifont_open` and reads every glyph back with `unifont_glyph` before it writes `OUT`, so the build gives no font that `term` cannot read.

## Updating

1. Download the new `unifont_all-VERSION.hex.gz` and its `.sig`, verify the signature with `gpgv` and the GNU keyring, put both here and delete the old ones.
2. Change `UNIFONT_HEX` and `UNIFONT_SHA256` in `mk/unifont.mk`, and `HEX_SHA256` and the release named in `mkfont.sh`'s comment.
3. Run `make unifont`, and `mkfont.sh` into `rach2/sys/display/vga_font.c`; review what changed in the console's font.
4. Correct the counts above, and run the host test.

## Tests

[`tests/host/unifont_hosttest.c`](../../tests/host/unifont_hosttest.c) packs fifteen lines of the 18.0.01 text and reads back a Latin, a Cyrillic, an Armenian, a braille, a CJK and a plane 1 glyph, the gaps between them, a run broken by a change of width and one broken at 65,535 glyphs. It checks that the converter refuses each kind of bad line and that `unifont_open` refuses a font with any field of its header or a run broken.
