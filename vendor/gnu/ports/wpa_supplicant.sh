# shellcheck shell=sh disable=SC2034 # build_port.sh reads the variables
# wpa_supplicant, wpa_cli and wpa_passphrase: the station's WPA, WPA2 and
# WPA3 client, with the musl system's features (dev-log/69-wifi/plan.md, 4c).
# It drives the card through nl80211 with libnl, keeps the SME in user space,
# and takes its TLS from the root's OpenSSL, whose TLS backend gives the
# certificate forms probe:// and hash://server/sha256/. Its build is its
# Makefile, configured by .config, which this recipe writes; the release's
# 802.11w is always built, and TKIP is unless CONFIG_NO_TKIP is set. The
# manual pages are the ones the release carries made from DocBook.
VERSION=2.12
DEPENDS="libnl openssl"

wpa_make()
{
    make -C "$SRC/wpa_supplicant" CC="$TARGET-gcc" PKG_CONFIG="$TARGET-pkg-config" \
        BINDIR=/usr/sbin LIBDIR=/usr/lib "$@"
}

port_build()
{
    cat >"$SRC/wpa_supplicant/.config" <<'EOF'
CONFIG_DRIVER_NL80211=y
CONFIG_LIBNL32=y
CONFIG_TLS=openssl
CONFIG_SAE=y
CONFIG_IEEE8021X_EAPOL=y
CONFIG_EAP_TLS=y
CONFIG_EAP_PEAP=y
CONFIG_EAP_TTLS=y
CONFIG_EAP_MSCHAPV2=y
CONFIG_EAP_GTC=y
CONFIG_PKCS12=y
CONFIG_CTRL_IFACE=y
CONFIG_BACKEND=file
CONFIG_BGSCAN_SIMPLE=y
CONFIG_WPA_CLI_EDIT=y
CONFIG_DEBUG_SYSLOG=y
EOF
    wpa_make
}

port_install()
{
    mkdir -p "$DEST/usr/sbin"
    wpa_make install DESTDIR="$DEST"
    for p in wpa_supplicant.8 wpa_cli.8 wpa_passphrase.8 wpa_background.8 wpa_supplicant.conf.5; do
        install -D -m 0644 "$SRC/wpa_supplicant/doc/docbook/$p" "$DEST/usr/share/man/man${p##*.}/$p"
    done
}
