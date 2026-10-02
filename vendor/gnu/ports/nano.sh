# shellcheck shell=sh disable=SC2034 # build_port.sh reads the variables
# GNU nano, the editor, with libmagic to choose a file's syntax. Its
# /etc/nanorc is the sample upstream installs as documentation, with the line
# that includes every syntax definition taken out of its comment, as
# Debian's has it.
VERSION=9.2
DEPENDS="ncurses file"
CONFIGURE_ARGS="--disable-nls --enable-utf8"

port_install()
{
    make install DESTDIR="$DEST"
    mkdir -p "$DEST/etc"
    sed 's|^# include /usr/share/nano/\*\.nanorc$|include /usr/share/nano/*.nanorc|' \
        doc/sample.nanorc >"$DEST/etc/nanorc"
    if ! grep -q '^include /usr/share/nano/\*\.nanorc$' "$DEST/etc/nanorc"; then
        echo "gnu: nano: the sample nanorc has no line including the syntax definitions" >&2
        exit 1
    fi
}
