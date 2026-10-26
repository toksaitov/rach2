#!/bin/sh
# launch.sh: the installed launcher, which runs `etude launch`.
# An enrolled endpoint's binding names the protected etude, which takes the
# native gate before it reads any site; otherwise the etude installed beside
# this file admits the command (tools/etude/Readme.md).
set -eu
authority=/etc/rach2/etude-authority
case $(uname -s) in MINGW*|MSYS*|UCRT*|CLANG*) authority='C:/ProgramData/rach2/etude-authority' ;; esac
if [ -e "$authority" ] || [ -L "$authority" ]; then
    program=$(sed -n 's/^program //p' "$authority")
    case $program in /*|[A-Za-z]:/*) ;; *) echo 'etude-launch: invalid authority program' >&2; exit 2 ;; esac
    exec "$program" authority launch "$@"
fi
exec "${0%/*}/etude" launch "$@"
