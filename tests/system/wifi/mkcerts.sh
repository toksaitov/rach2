#!/bin/sh
#
# mkcerts.sh: make the certificates of the Linux AP guest's EAP server again,
# into tests/system/wifi/certs. The build does not run it; the certificates
# are committed, so that a fingerprint a suite pins stays the same.
#
# Usage: mkcerts.sh
#
# server.pem is the self-signed certificate of a university's RADIUS server,
# as the owner's network presents it; server2.pem has the same subject and
# another key, the certificate of the changed-certificate row. client.pem is
# the certificate EAP-TLS's station shows, signed by clientca.pem, which the
# server trusts; the CA's key is not kept. Every certificate is valid for a
# hundred years from the day it was made, and every key is RSA 2048 without
# a passphrase. It needs OpenSSL 1.1.1 or later.

set -e

DIR="$(cd "$(dirname "$0")" && pwd)/certs"
work=
trap '[ -z "$work" ] || rm -rf "$work"' EXIT
work="$(mktemp -d "${TMPDIR:-/tmp}/rach2-mkcerts.XXXXXX")"
mkdir -p "$DIR"
days=36525
subject='/C=KG/O=Rach2 University/CN=radius.university.test'

# self_signed NAME: a self-signed server certificate NAME.pem and its key.
self_signed()
{
    openssl req -x509 -newkey rsa:2048 -nodes -sha256 -days "$days" \
        -subj "$subject" -keyout "$DIR/$1.key" -out "$DIR/$1.pem" \
        -addext 'basicConstraints=critical,CA:TRUE' \
        -addext 'keyUsage=critical,digitalSignature,keyEncipherment,keyCertSign' \
        -addext 'extendedKeyUsage=serverAuth' 2>/dev/null
}

self_signed server
self_signed server2
openssl req -x509 -newkey rsa:2048 -nodes -sha256 -days "$days" \
    -subj '/C=KG/O=Rach2 University/CN=Rach2 University Client CA' \
    -keyout "$work/clientca.key" -out "$DIR/clientca.pem" 2>/dev/null
openssl req -new -newkey rsa:2048 -nodes -sha256 \
    -subj '/C=KG/O=Rach2 University/CN=client@wifi.test' \
    -keyout "$DIR/client.key" -out "$work/client.csr" 2>/dev/null
printf 'extendedKeyUsage=clientAuth\n' > "$work/client.ext"
openssl x509 -req -sha256 -days "$days" -in "$work/client.csr" \
    -CA "$DIR/clientca.pem" -CAkey "$work/clientca.key" -set_serial 1 \
    -extfile "$work/client.ext" -out "$DIR/client.pem" 2>/dev/null
for c in server server2 client; do
    printf '%s: ' "$c"
    openssl x509 -in "$DIR/$c.pem" -noout -fingerprint -sha256
done
