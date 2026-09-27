#!/bin/sh
# Compile fixed, faulty and restored utility sources in a private directory.
set -eu

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"
mkdir -p build/tests
work=$(mktemp -d "$repo/build/tests/account-proof.XXXXXX")
cp -R utils "$work/utils"
cp -R "$work/utils" "$work/saved"
child=

stop_child()
{
    if [ -n "$child" ]; then
        kill -TERM "-$child" 2>/dev/null || :
        wait "$child" 2>/dev/null || :
        while kill -0 "-$child" 2>/dev/null; do
            sleep 1
        done
        child=
    fi
}

restore()
{
    for file in pwauth.c login.c su.c sudo.c init.c nc.c; do
        cp "$work/saved/$file" "$work/utils/$file"
        touch "$work/utils/$file"
    done
}

cleanup()
{
    stop_child
    restore
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

run_phase()
{
    phase=$1
    test=$2
    expected=$3
    message=$4
    binary="$work/$phase"
    if ! "${CC:-cc}" -std=c99 -D_DEFAULT_SOURCE -D_FORTIFY_SOURCE=0 -DRACH2_HOSTTEST \
        -g -O1 -ffreestanding -Wall -Wextra -Werror \
        -fsanitize=address,undefined -fno-sanitize-recover=all \
        -I "$work/utils" -I tests/host/compat \
        "tests/host/${test}_hosttest.c" -o "$binary" >"$binary.compile.log" 2>&1; then
        cat "$binary.compile.log" >&2
        exit 1
    fi
    cksum "$work/utils/"*.c "$binary" >"$binary.cksum"
    perl -e 'setpgrp(0, 0); alarm 15; exec @ARGV or die "account-mutation: exec: $!\n"' \
        "$binary" >"$binary.log" 2>&1 &
    child=$!
    status=0
    wait "$child" || status=$?
    stop_child
    cat "$binary.log"
    [ "$status" -eq "$expected" ] && grep -Fq "$message" "$binary.log" || exit 1
    echo "account-mutation: result: $phase status $status"
}

echo "account-mutation: logs: $work"
for test in pwauthfault accounttools initpolicy ncwrite; do
    run_phase "$test-fixed" "$test" 0 "$test-hosttest: result: pass"
done
for fault in pid-nul pid-tail echo tty-detect tty-error lock-cleanup temp-cleanup \
    login-eof login-fds su-fds sudo-fds sudo-line init-redirect init-network \
    init-block init-signals init-sshd-block init-sshd-defaults init-sshd-mask \
    init-login-timeout init-login-keep init-login-parent ssh-suffix nc-socket nc-stdout; do
    python3 - "$work/utils" "$fault" <<'PY'
from pathlib import Path
import sys

root = Path(sys.argv[1])
fault = sys.argv[2]
changes = {
    'pid-nul': ('pwauth.c', "for (i = 0; i < n; i++) {\n            if (!suffix",
                "for (i = 0; i < n; i++) {\n            if (buf[i] == 0) { *out = value; return digits > 0; }\n            if (!suffix"),
    'pid-tail': ('pwauth.c', '    }\n    if (digits == 0 || value <= 0)',
                '        break;\n    }\n    if (digits == 0 || value <= 0)'),
    'echo': ('pwauth.c', 'Tty_failed = 1;\n            taken = -1;', 'taken = 1;'),
    'tty-detect': ('pwauth.c', 'static int pwauth_prepare_tty(void)\n{',
                   'static int pwauth_prepare_tty(void)\n{\n    if (!isatty(0)) { return 0; }'),
    'tty-error': ('pwauth.c', 'return errno == ENOTTY ? 0 : -1;', 'return 0;'),
    'init-block': ('init.c', 'return sigprocmask(SIG_BLOCK, &shutdown, saved);',
                   'return sigprocmask(SIG_BLOCK, NULL, saved);'),
    'init-sshd-block': ('init.c', '    if (shutdown_block(&saved) != 0) {\n        return -1;\n    }',
                        '    if (sigprocmask(SIG_BLOCK, NULL, &saved) != 0) {\n        return -1;\n    }'),
    'init-sshd-defaults': ('init.c',
                           '    if (shutdown_defaults(&saved) != 0) {\n        _exit(126);\n    }\n    service_output();\n    {',
                           '    service_output();\n    {'),
    'init-sshd-mask': ('init.c', '        (void)sigprocmask(SIG_SETMASK, &saved, NULL);\n        return pid;',
                       '        return pid;'),
    'lock-cleanup': ('pwauth.c', 'failed = unlink(lock_path) != 0 && errno != ENOENT;',
                     '(void)unlink(lock_path);\n        failed = 0;'),
    'login-eof': ('login.c', 'if (len == 0 && !over) {\n                return -1;\n            }\n            break;',
                  'return len > 0 ? 0 : -1;'),
    'login-fds': ('login.c', '#define LOGIN_MAX_FDS 1024', '#define LOGIN_MAX_FDS 16'),
    'su-fds': ('su.c', '#define MAX_FD 1024', '#define MAX_FD 64'),
    'sudo-fds': ('sudo.c', '#define MAX_FD 1024', '#define MAX_FD 64'),
    'sudo-line': ('sudo.c', "while ((c = fgetc(f)) != EOF && c != '\\n') {",
                  "c = 0;\n        while (len < size - 1 && (c = fgetc(f)) != EOF && c != '\\n') {"),
    'init-login-timeout': ('init.c', '        (void)setenv("LOGIN_TIMEOUT", "0", 1);\n', ''),
    'init-login-keep': ('init.c', 'setenv("LOGIN_TIMEOUT", "0", 1)', 'setenv("LOGIN_TIMEOUT", "0", 0)'),
    'ssh-suffix': ('init.c', "} else if (c != ' ' && c != '\\t' && c != '\\r' && c != '\\v' && c != '\\f') {",
                   '} else if (0) {'),
    'nc-socket': ('nc.c', 'write_all(fd, buf, (size_t)n)', 'write(fd, buf, (size_t)n)'),
    'nc-stdout': ('nc.c', 'write_all(STDOUT_FILENO, buf, (size_t)m)', 'write(STDOUT_FILENO, buf, (size_t)m)'),
}
if fault in changes:
    name, old, new = changes[fault]
    path = root / name
    text = path.read_text()
    if text.count(old) != 1:
        raise SystemExit('account-mutation: source pattern is not unique: ' + fault)
    path.write_text(text.replace(old, new))
elif fault == 'init-signals':
    path = root / 'init.c'
    text = path.read_text()
    for signal in ('SIGUSR1', 'SIGUSR2', 'SIGTERM'):
        old = 'sigaction(' + signal + ', &action, NULL) != 0'
        assert text.count(old) == 1
        text = text.replace(old, '0')
    path.write_text(text)
elif fault == 'init-login-parent':
    path = root / 'init.c'
    text = path.read_text()
    child = '        (void)setenv("LOGIN_TIMEOUT", "0", 1);\n'
    start = 'static pid_t spawn_shell(void)\n{\n    pid_t pid = fork();'
    assert text.count(child) == 1 and text.count(start) == 1
    text = text.replace(child, '').replace(start,
        'static pid_t spawn_shell(void)\n{\n    pid_t pid = (setenv("LOGIN_TIMEOUT", "0", 1), fork());')
    path.write_text(text)
elif fault == 'temp-cleanup':
    path = root / 'pwauth.c'
    text = path.read_text()
    start = text.index('{', text.index('static int cred_drop_temp('))
    end = text.index('\n}', start) + 2
    text = text[:start] + '''{
    if (Txn_tmp[0] != '\\0') {
        (void)unlink(Txn_tmp);
        Txn_tmp[0] = '\\0';
    }
    return 0;
}''' + text[end:]
    path.write_text(text)
elif fault == 'init-redirect':
    path = root / 'init.c'
    text = path.read_text()
    start = text.index('{', text.index('static void service_output('))
    end = text.index('\n}', start) + 2
    text = text[:start] + '''{
    int sink = service_sink();
    if (service_null_fd >= 0) { (void)dup2(service_null_fd, 0); }
    if (sink >= 0) { (void)dup2(sink, 1); (void)dup2(sink, 2); }
}''' + text[end:]
    path.write_text(text)
elif fault == 'init-network':
    path = root / 'init.c'
    text = path.read_text()
    start = text.index('static void start_network(')
    end = text.index('\n}', start)
    body = text[start:end]
    assert body.count('service_output();') == 1
    path.write_text(text[:start] + body.replace('service_output();', '') + text[end:])
else:
    raise SystemExit('account-mutation: unknown fault')
PY
    case $fault in
        pid-nul) test=pwauthfault; message='embedded NUL rejected' ;;
        pid-tail) test=pwauthfault; message='suffix beyond first buffer rejected' ;;
        echo) test=pwauthfault; message='initial echo failure stops input' ;;
        tty-detect|tty-error) test=pwauthfault; message='terminal mode error stops input' ;;
        lock-cleanup) test=pwauthfault; message='lock rollback reports unlink failure' ;;
        temp-cleanup) test=pwauthfault; message='temporary cleanup reports unlink failure' ;;
        login-eof) test=accounttools; message='partial EOF terminates name' ;;
        login-fds) test=accounttools; message='login closes high descriptors' ;;
        su-fds) test=accounttools; message='su closes high descriptors before identity' ;;
        sudo-fds) test=accounttools; message='sudo closes high descriptors before identity' ;;
        sudo-line) test=accounttools; message='long comment cannot authorize uid' ;;
        init-redirect) test=initpolicy; message='service cannot return after redirection failure' ;;
        init-network) test=initpolicy; message='network setup requires redirected descriptors' ;;
        init-block) test=initpolicy; message='early network signal terminates worker' ;;
        init-signals) test=initpolicy; message='ifup signal terminates worker' ;;
        init-sshd-block|init-sshd-defaults) test=initpolicy
            message="a shutdown signal ends the SSH service's child, not init's handler" ;;
        init-sshd-mask) test=initpolicy; message="the SSH service's fork restores init's signal mask" ;;
        init-login-timeout|init-login-keep) test=initpolicy; message='the console login waits without a timeout' ;;
        init-login-parent) test=initpolicy; message="init's own environment keeps its LOGIN_TIMEOUT" ;;
        ssh-suffix) test=initpolicy; message='SSH trailing text rejected' ;;
        nc-socket|nc-stdout) test=ncwrite; message='complete partial writes on both outputs' ;;
    esac
    run_phase "$fault-mutated" "$test" 1 "$message"
    restore
    run_phase "$fault-restored" "$test" 0 "$test-hosttest: result: pass"
    diff -q "$work/saved" "$work/utils"
done
echo 'account-mutation: result: pass: 25 compiled faults failed; restored tests passed'
