#!/bin/sh
# installer_volume.sh: copy a file off the exFAT volume of an install stick
# for the installer suite's machine cases, after a check of the whole volume.
#
# Usage: installer_volume.sh FIXTURE IMAGE START PATH OUT
#
# FIXTURE is mkexfatfixture, which the case gives as an artifact; IMAGE the
# stick's image, START the volume's first sector, PATH the file's path on the
# volume, and OUT the file its bytes are written to. The fixture writes the
# file to its standard output, which the kept output of a host tool would cut
# short, so it runs here, from an executable copy beside OUT. The exit status
# is the fixture's: 0 when the volume checks clean and has the file.
set -eu

if [ "$#" -ne 5 ]; then
    echo "usage: installer_volume.sh FIXTURE IMAGE START PATH OUT" >&2
    exit 2
fi
fixture="$5.fixture"
trap 'rm -f "$fixture"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
cp "$1" "$fixture"
chmod 700 "$fixture"
"$fixture" --check "$2" "$3" --read "$4" >"$5"
