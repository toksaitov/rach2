#!/bin/bash
# Quick environment checks referenced by the report (read-only, safe to re-run).
echo "--- R2-006 vDSO mapped?"; grep -cE '\[vdso\]' /proc/self/maps | sed 's/^0$/NO vdso mapping/'
echo "--- R2-007 RLIMIT_NOFILE (soft/hard) and can root raise it?"; ulimit -Sn; ulimit -Hn; (ulimit -n 65536) 2>&1 | head -1 || true
echo "--- R2-012 sysctl writability as root"; for f in /proc/sys/fs/inotify/max_user_watches /proc/sys/vm/swappiness; do v=$(cat $f); (echo "$v" > $f) 2>/dev/null && echo "$f writable" || echo "$f NOT writable"; done; echo "sysctl count: $(sysctl -a 2>/dev/null | wc -l)"
echo "--- R2-021 ASLR (stack start should differ between runs)"; for i in 1 2; do grep -m1 '\[stack\]' /proc/self/maps | cut -d' ' -f1; done
echo "--- R2-022 ss -xp (unix sock_diag with process info)"; ss -xp 2>&1 | head -2
echo "--- R2-024 CPU topology"; lscpu 2>&1 | grep -E 'Thread|Core\(s\)|Socket'; grep -m1 -E 'MHz' /proc/cpuinfo || echo "no 'cpu MHz' in /proc/cpuinfo"
echo "--- R2-029 RLIMIT_NPROC"; ulimit -Su; ulimit -Hu
echo "--- R2-016 chromium launcher"; file /usr/bin/chromium-headless-shell; grep -c '^exec ' /usr/bin/chromium-headless-shell | sed 's/^0$/launcher does NOT exec/'
echo "--- R2-019 missing tools"; for c in node npm npx pip pip3 uv pipx rg fd shellcheck clang clang-format valgrind ltrace lldb perf ninja meson git-lfs docker podman bwrap socat pdftotext magick ffmpeg pandoc java go cargo; do command -v $c >/dev/null 2>&1 || printf '%s ' $c; done; echo
