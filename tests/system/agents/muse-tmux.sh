# shellcheck shell=bash
# muse-tmux: the same scripted Muse session in its interactive screen.
# bound: 600
# hosts: muse.fakeapi.test
# script: fakeapi/muse.json
# script-filter: muse_verify.py
# package: muse
# verifier: muse_verify.py

# shellcheck source=tests/system/agents/muse.sh
. /tmp/agents/muse.sh

row() {
    muse_session tmux
}
