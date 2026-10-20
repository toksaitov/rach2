# shellcheck shell=bash
# gh: the version its recipe pins, the program the path finds and its
# license beside it, `gh auth status` with no account signed in, and one
# HTTPS request with no account, to openssl's test server on this machine as
# a GitHub Enterprise host, which gh refuses until SSL_CERT_FILE names the
# server's certificate. curl tries again while the server starts. Last, an
# invalid token in GH_TOKEN, sent to api.github.com, which is the fake API
# server here, gets GitHub's answer to a bad token, 401, and gh reports it;
# a native run leaves that out, since only QEMU's network reaches the fake
# server by that name.
# bound: 300
# hosts: api.github.com

row() {
    local server https
    https=$(port 4433)

    output "gh --version" gh --version
    has "the version" "$(printf '%s\n' "$OUT" | head -n 1)" "gh version $VERSION ("
    same "the program" "$(readlink -f "$(type -P gh)")" "/opt/gh/bin/gh"
    has "the license" "$(head -n 1 /opt/gh/LICENSE)" "MIT License"
    status "gh auth status" 1 gh auth status
    has "gh auth status" "$OUT" "You are not logged into any GitHub hosts"

    output "a certificate" openssl req -x509 -newkey ed25519 -keyout "$WORK/key.pem" \
        -out "$WORK/cert.pem" -days 1 -nodes -subj /CN=localhost -addext subjectAltName=DNS:localhost
    openssl s_server -quiet -accept "127.0.0.1:$https" -key "$WORK/key.pem" -cert "$WORK/cert.pem" \
        -www > "$WORK/server.log" 2>&1 &
    server=$!
    output "the server" curl -sS -o /dev/null --retry 120 --retry-connrefused --retry-delay 1 \
        --cacert "$WORK/cert.pem" "https://localhost:$https/"
    status "gh api, the certificate untrusted" 1 env GH_HOST="localhost:$https" gh api zen
    has "gh api, the certificate untrusted" "$OUT" "certificate signed by unknown authority"
    output "gh api" env GH_HOST="localhost:$https" SSL_CERT_FILE="$WORK/cert.pem" gh api zen
    has "gh api over TLS" "$OUT" "Protocol  : TLSv1.3"
    kill "$server"
    wait "$server" 2> /dev/null
    [ -z "$NATIVE" ] || return 0

    status "gh api with an invalid token" 1 env GH_TOKEN=invalid SSL_CERT_FILE="$CA" gh api user
    has "gh api with an invalid token" "$OUT" "gh: Bad credentials (HTTP 401)"
}
