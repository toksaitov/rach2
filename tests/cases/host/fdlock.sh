#!/bin/sh
# fdlock.sh: the namespace and relocated-handle checks of the fd host test.
set -eu
cd "${ETUDE_TREE:-.}"
printf '@etude 1 begin %s\n' "${ETUDE_CASE:-guest.fdlock.host}"
status=0
build/tests/host/fd_hosttest || status=$?
if [ "$status" -eq 0 ]; then
    printf '@etude 1 assert locking pass\n'
else
    printf '@etude 1 assert locking fail the fd host test failed\n'
fi
printf '@etude 1 end 1\n'
exit "$status"
