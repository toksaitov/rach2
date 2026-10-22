#!/bin/sh
#
# drive.sh: the program of the drive case, tests/cases/host.drive.json. It
# runs tests/system/drivetest.sh, which writes the case's report records.
#
# The test stays beside the drivers it tests, since its fake machines sleep
# as the stimulus and it passes the harness the `wait=` step that the harness
# must refuse, forms that tools/verify/suitecheck.sh refuses in the sources of
# tests/cases.
#
# Usage, in the root of a built tree, or anywhere with ETUDE_TREE set to it:
#     sh tests/cases/host/drive.sh
#

exec sh "${ETUDE_TREE:-.}/tests/system/drivetest.sh"
