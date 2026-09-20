#!/bin/sh
# Compose recovery keys from the builder's keys and an operator's public file.
set -eu
if [ "$#" -lt 2 ] || [ "$#" -gt 3 ]; then
    echo "gen-recovery-key: usage: BASE OUTPUT [EXTRA-PUBLIC-KEYS]" >&2
    exit 2
fi
BASE=$1
OUT=$2
EXTRA=${3:-}
if [ -z "$EXTRA" ] && [ -f "$OUT" ]; then
    exit 0
fi
if [ -n "$EXTRA" ] && [ ! -f "$EXTRA" ]; then
    echo "gen-recovery-key: extra public-key file is missing" >&2
    exit 1
fi
umask 077
TMP=$(mktemp "$OUT.XXXXXX")
trap 'rm -f "$TMP" "$TMP.line"' 0
trap 'exit 130' INT
trap 'exit 143' TERM HUP
cat "$BASE" > "$TMP"
if [ -n "$EXTRA" ]; then
    printf '\n' >> "$TMP"
    count=0
    while IFS= read -r line || [ -n "$line" ]; do
        case "$line" in
            ''|'#'*) continue ;;
            ssh-*|ecdsa-sha2-*|sk-*) ;;
            *)
                echo "gen-recovery-key: extra file needs plain public keys" >&2
                exit 1
                ;;
        esac
        case "$line" in
            *'PRIVATE KEY'*)
                echo "gen-recovery-key: private key in extra file" >&2
                exit 1
                ;;
        esac
        printf '%s\n' "$line" > "$TMP.line"
        if ! ssh-keygen -l -f "$TMP.line" >/dev/null 2>&1; then
            echo "gen-recovery-key: invalid public key in extra file" >&2
            exit 1
        fi
        cat "$TMP.line" >> "$TMP"
        count=$((count + 1))
    done < "$EXTRA"
    if [ "$count" -eq 0 ]; then
        echo "gen-recovery-key: extra file has no public keys" >&2
        exit 1
    fi
fi
if [ ! -f "$OUT" ] || ! cmp -s "$TMP" "$OUT"; then
    mv "$TMP" "$OUT"
fi
