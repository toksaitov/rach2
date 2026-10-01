# shellcheck shell=sh disable=SC2034 # build_port.sh reads the variables
# The time zone database, which glibc reads under /usr/share/zoneinfo for
# the zone TZ or /etc/localtime names. The root's own zic, glibc's, compiles
# it there, which the build host runs through the root's loader, as the
# runtime's recipe runs localedef: the regions and `backward`, which keeps
# the older names, such as US/Eastern. The tables tzselect reads are copied
# beside them. The tarball has no directory of its own, so the recipe
# unpacks it in the port's work directory.
VERSION=2026e
SOURCE=

port_build()
{
    tar -xf "$("$DIR/fetch.sh" --path tzdata)"
}

port_install()
{
    zoneinfo="$DEST/usr/share/zoneinfo"
    mkdir -p "$zoneinfo"
    "$ROOT/lib/ld-linux.so.2" --library-path "$ROOT/lib:$ROOT/usr/lib" "$ROOT/usr/sbin/zic" \
        -d "$zoneinfo" africa antarctica asia australasia europe northamerica southamerica \
        etcetera backward
    cp iso3166.tab zone.tab zone1970.tab "$zoneinfo/"
}
