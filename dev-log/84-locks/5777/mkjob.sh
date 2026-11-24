#!/bin/sh
# mkjob.sh N MODE: print the guest script that drive.py runs as root on the
# GNU root: it writes race.c and loop2.sh to /tmp, builds race.so with the
# root's GCC and runs `loop2.sh N MODE` as the test account.
#
#   sh mkjob.sh 200 plain > build/tmp/job.sh
#   python3 -I dev-log/84-locks/5777/drive.py build/x86_64/rach2/kernel \
#       build/x86_64/images/gnu-test.disk build/tmp/job.sh 3
here=$(cd "$(dirname "$0")" && pwd)
[ $# -eq 2 ] || { echo "usage: mkjob.sh N plain|trace|delay" >&2; exit 2; }
echo "cat > /tmp/race.c <<'RACEEOF'"
cat "$here/race.c"
echo "RACEEOF"
echo "cat > /tmp/loop2.sh <<'LOOPEOF'"
cat "$here/loop2.sh"
echo "LOOPEOF"
echo "chmod 644 /tmp/race.c /tmp/loop2.sh"
echo "cc -shared -fPIC -O2 -o /tmp/race.so /tmp/race.c -ldl -lpthread || echo 'race.so: build failed'"
echo "uname -a; grep -c ^processor /proc/cpuinfo"
echo "su -s /bin/sh test -c 'sh /tmp/loop2.sh $1 $2'"
