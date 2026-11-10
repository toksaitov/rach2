# shellcheck shell=sh disable=SC2154 # install_root.sh sets the variables
#
# install_games.sh: install the games layer (Readme.md) over the GNU root.
#
# install_root.sh sources this once the desktop closure, Venus, the i386
# layer, Proton and the test suites are in TREE, so that it uses that
# script's TREE, OUT, WORK, install_debian and needs, and a failure here stops
# the root. It installs the games closure that `make gnu-games` staged in
# OUT/debian/games, less the packages SKIP and the desktop closure already
# account for, refusing a file the root already has; then it refuses the root
# when a library that a program or library of the layer needs is in none of
# the root's library directories, since nothing resolved the layer's
# libraries against the root.
#

install_debian games

# Each ELF file the layer installed; a link is left to its target.
while read -r path; do
    f="$TREE/${path#./}"
    if [ -L "$f" ] || [ ! -f "$f" ] ||
        [ "$(od -An -tx1 -N4 "$f" | tr -d ' \n')" != 7f454c46 ]; then
        continue
    fi
    needs "the games layer's /${path#./}" "$f" lib usr/lib "usr/lib/$GNU_MULTIARCH"
done < "$WORK/new"
