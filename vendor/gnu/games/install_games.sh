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
# libraries against the root. On x86-64 it adds the Proton launcher and the
# native games' replays, and on a root with /usr/games puts it on the path.
# With GNU_WESTON 1 it puts the rebuilt Weston in place of the desktop
# closure's.
#

install_debian games

# Each ELF file the layer installed; a link is left to its target. A file's
# RUNPATH or RPATH directories are searched as well, with $ORIGIN taken as
# the file's own directory, since 0 A.D.'s engine finds its private libraries
# in /usr/lib/games/0ad by its RUNPATH. The loader's directory is searched
# too, since 0 A.D.'s engine names the loader, which resolves its own name.
while read -r path; do
    f="$TREE/${path#./}"
    if [ -L "$f" ] || [ ! -f "$f" ] ||
        [ "$(od -An -tx1 -N4 "$f" | tr -d ' \n')" != 7f454c46 ]; then
        continue
    fi
    origin=$(dirname "${path#./}")
    runpath=$(LC_ALL=C readelf -d "$f" |
        sed -n 's/.*(R\(UN\)\{0,1\}PATH).*\[\(.*\)\]$/\2/p' | tr ':' '\n' |
        sed -e "s|^\\\$ORIGIN|/$origin|" -e "s|^\\\${ORIGIN}|/$origin|" -e 's|^/||' | tr '\n' ' ')
    # shellcheck disable=SC2086 # the directories are words without blanks
    needs "the games layer's /${path#./}" "$f" "$(dirname "${GNU_LOADER#/}")" lib usr/lib \
        "usr/lib/$GNU_MULTIARCH" $runpath
done < "$WORK/new"

# Debian's games are in /usr/games, which Debian's /etc/profile puts on an
# ordinary user's path; here every login shell gets it, after the rest.
if [ -d "$TREE/usr/games" ]; then
    if [ -e "$TREE/etc/profile.d/games.sh" ]; then
        echo 'gnu: games: /etc/profile.d/games.sh already exists' >&2
        exit 1
    fi
    mkdir -p "$TREE/etc/profile.d"
    # shellcheck disable=SC2016 # the shell that reads the file expands it
    printf '%s\n' 'PATH=$PATH:/usr/local/games:/usr/games' > "$TREE/etc/profile.d/games.sh"
    chmod 0644 "$TREE/etc/profile.d/games.sh"
fi

if [ "$RACH2_ARCH" = x86_64 ]; then
    for command in rach2-game crysis.sh; do
        if [ -e "$TREE/usr/bin/$command" ] || [ -L "$TREE/usr/bin/$command" ]; then
            echo "gnu: games: /usr/bin/$command already exists" >&2
            exit 1
        fi
    done
    install -m 0755 "$DIR/games/rach2-game.sh" "$TREE/usr/bin/rach2-game"
    install -m 0755 "$DIR/games/crysis.sh" "$TREE/usr/bin/crysis.sh"
    mkdir -p "$TREE/usr/share/rach2/crysis"
    install -m 0644 "$DIR/games/game.sha256" "$DIR/games/crysis-installers.sha256" \
        "$TREE/usr/share/rach2/crysis/"
    # The native games' fixed replays, which the games-native suite and the
    # Lenovo's runs play.
    if [ -e "$TREE/usr/share/games/rach2" ] || [ -L "$TREE/usr/share/games/rach2" ]; then
        echo 'gnu: games: /usr/share/games/rach2 already exists' >&2
        exit 1
    fi
    mkdir -p "$TREE/usr/share/games/rach2"
    cp -R "$DIR/games/replays" "$TREE/usr/share/games/rach2/replays"
    chmod -R u=rwX,go=rX "$TREE/usr/share/games/rach2"
fi

# The rebuilt Weston (Readme.md, Weston): every file the desktop closure's
# weston and libweston-14-0 installed goes, so that the weston part, built
# from the same source package with Rach2's patches, comes in whole.
if [ "${GNU_WESTON:-}" = 1 ]; then
    for p in weston libweston-14-0; do
        list="$OUT/work/debian/desktop/files/$p"
        if [ ! -f "$list" ]; then
            echo "gnu: games: refused: the desktop closure has no $p, which the weston part replaces" >&2
            exit 1
        fi
        while read -r path; do
            f="$TREE/${path#./}"
            if [ -L "$f" ] || [ -f "$f" ]; then
                rm "$f"
            fi
        done < "$list"
    done
    install_part weston
    # weston's own library is in its RUNPATH, usr/lib/MULTIARCH/weston.
    needs "the rebuilt weston" "$TREE/usr/bin/weston" lib usr/lib "usr/lib/$GNU_MULTIARCH" \
        "usr/lib/$GNU_MULTIARCH/weston"
    needs "the rebuilt libweston" "$TREE/usr/lib/$GNU_MULTIARCH/libweston-14.so.0.0.2" \
        lib usr/lib "usr/lib/$GNU_MULTIARCH"
fi
