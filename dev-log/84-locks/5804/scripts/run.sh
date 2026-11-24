#!/bin/sh
# t5804: run drive.py as one claim of the site's ledger, the processors and
# memory of a 4-CPU, 8 GiB guest, as loop s's runs of #5798 claim.
#   run.sh RUNDIR STEP:COUNT...
set -eu
tree=/home/toksaitov-m/rach2
site=/etc/rach2/etude-site
field() { sed -n "s/^$1 //p" "$site"; }
id=t5804-m-$(date +%s)
exec "$tree/build/tools/etude/etude" admit -c -d "$(field locks)" -w "$(field wait)" \
    "$(field ledger)" "$id" "$(id -un)" shared 4000,9663676416,1 \
    "$(field capacity)" 43200 \
    /usr/bin/env T5804_CLAIM="$id" python3 -u "$tree/build/tmp/t5804/drive.py" "$@"
