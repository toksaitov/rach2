#!/bin/sh
# Run the rows of the Wine probes in the Debian guest on the virgl profile,
# with the pinned GE-Proton at /opt/proton, and write Linux's results to OUT.
# Usage: reference.sh OUT [ROW...]; every row when none is named.
set -eu

if [ "${1:-}" = guest ]; then
    out=/tmp/games-reference
    trap 'exit 129' HUP
    trap 'exit 130' INT
    trap 'exit 143' TERM
    sudo mkdir -p /opt/proton
    sudo tar -xzf /tmp/proton.tar.gz -C /opt/proton --strip-components=1
    rm -rf /tmp/games "$out"
    mkdir -p /tmp/games "$out"
    tar -xf /tmp/bundle.tar -C /tmp/games
    mkdir -p /tmp/.X11-unix
    shift
    [ "$#" -gt 0 ] || set -- prefix draw-64 draw-32 maps
    status=0
    for row in "$@"; do
        GAMES_REFERENCE=$out/$row.json GAMES_LAUNCHER='sh /tmp/games/rach2-game.sh' \
            python3 /tmp/games/run.py "$row" "$out/$row" || status=1
    done
    python3 - "$out" <<'PY'
import json, sys
from pathlib import Path
out = Path(sys.argv[1])
rows = {}
for p in sorted(out.glob("*.json")):
    rows.update(json.loads(p.read_text()))
ref = {"draw": {}, "maps": {}, "record": {}}
for row, results in rows.items():
    ref["record"][row] = results.pop("record", None)
    for name, value in results.items():
        if row.startswith("draw-"):
            ref["draw"][name] = value["crc32"]
        elif row == "maps":
            ref["maps"][name] = value["coverage"]
(out / "linux.json").write_text(json.dumps(ref, indent=2, sort_keys=True) + "\n")
PY
    # The status travels in the archive, which debian-ref.sh copies out only
    # after a command that succeeded, so that a failed row's logs come too.
    echo "$status" > "$out/status"
    tar -czf /tmp/games-reference.tar.gz -C "$out" .
    exit 0
fi

out=$1
shift
here=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$here/../../.." && pwd)
GNU=$REPO/vendor/gnu
# shellcheck source=../../../vendor/gnu/debian/fetch_asset.sh
. "$GNU/debian/fetch_asset.sh"
work=
trap '[ -z "$work" ] || rm -rf "$work"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
mkdir -p "$REPO/build/tmp" "$out"
work=$(mktemp -d "$REPO/build/tmp/rach2-games-reference.XXXXXX")
make -s -C "$REPO" ARCH=x86_64 build/tests/system/games/bundle.tar
cp "$REPO/build/tests/system/games/bundle.tar" "$work/bundle.tar"
cp "$(asset ge-proton)" "$work/proton.tar.gz"
ref=$REPO/tests/graphics/debian-ref/debian-ref.sh
sh "$ref" prepare -n
status=0
sh "$ref" run -p virgl -c 4 -m 8192 -d -r "$out/record" -i "$work/bundle.tar" \
    -i "$work/proton.tar.gz" -i "$here/reference.sh" -o "$work/games-reference.tar.gz" \
    "sh /tmp/reference.sh guest $*" || status=$?
[ "$status" -eq 0 ] || exit "$status"
tar -xzf "$work/games-reference.tar.gz" -C "$out"
exit "$(cat "$out/status")"
