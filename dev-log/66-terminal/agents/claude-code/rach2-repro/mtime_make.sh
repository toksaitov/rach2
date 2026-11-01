#!/bin/sh
# R2-002: a source edited in the same second as the previous build is not rebuilt (1 s mtime granularity).
# Run on the filesystem under test (default: current directory). Expect "after edit rc=2".
d=$(cd "$(mktemp -d "${1:-.}/mtime.XXXXXX")" && pwd) && cd "$d" || exit 1
printf 'int main(){return 1;}\n' > m.c; printf 'm: m.c\n\tcc -o m m.c\n' > Makefile
make -s; ./m; echo "first build rc=$?"
printf 'int main(){return 2;}\n' > m.c; make -s; ./m; echo "after edit rc=$? (2 = correct, 1 = stale binary)"
python3 -c "import os; print('st_mtime_ns of m.c:', os.stat('m.c').st_mtime_ns, '(ends in 000000000 => 1 s granularity)')"
cd / && rm -r "$d"
