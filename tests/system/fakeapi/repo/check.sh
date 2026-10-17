#!/bin/sh
# check.sh: the test of the agents' scripts, which the agent runs before its
# change, when it fails, and after, when it passes.
if grep -qx 'hello, rach2' greeting.txt; then
    echo 'check: ok'
else
    echo "check: FAIL: greeting.txt says $(cat greeting.txt)"
    exit 1
fi
