#!/bin/sh
# Produce and export exactly one admitted current-commit collection selection.
set -eu
[ "$#" -ge 9 ] || exit 2
purpose=$1 tree=$2 out=$3 arches=$4 run=$5 site=$6 etude=$7 accel=$8
shift 8
case $purpose in push|ci|release) ;; *) exit 2 ;; esac
[ -n "${ETUDE_ADMITTED:-}" ] && [ "$ETUDE_ADMITTED" = "$site" ] || exit 2
[ "$(git -C "$tree" rev-parse HEAD)" = "$ETUDE_ROUTINE_COMMIT" ] || exit 2
[ ! -e "$out/producer.json" ] || exit 2
ETUDE_PREPARATION_RUN_ID=$run ETUDE_PREPARATION_RECEIPT=$out/preparation.receipt.json
ETUDE_PREPARATION_COMMIT=$ETUDE_ROUTINE_COMMIT ETUDE_PREPARATION_EXPORT=1
export ETUDE_PREPARATION_RUN_ID ETUDE_PREPARATION_RECEIPT ETUDE_PREPARATION_COMMIT
RACH2_QEMU_ACCEL=$accel
export ETUDE_PREPARATION_EXPORT RACH2_QEMU_ACCEL
if [ "$purpose" = release ]; then ETUDE_PREPARATION_CLEAN=1; export ETUDE_PREPARATION_CLEAN; fi
"$etude" prepare collection -t "$tree" -a "$arches" -p "$out/prepare.record" "$@"
python3 "$tree/tools/etude/preparation_import.py" export --tree "$tree" --commit "$ETUDE_ROUTINE_COMMIT" \
    --record "$out/prepare.record" --receipt "$out/preparation.receipt.json" --etude "$etude" \
    --output "$out/producer.json"
