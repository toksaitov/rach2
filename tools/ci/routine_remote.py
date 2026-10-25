"""Preparation client of the existing native etude serve protocol.

The responder's executor owns all remote work. Closing this connection
cancels and reaps that work; an SSH subprocess alone is not cleanup proof.
"""
import base64
import hashlib
import json
import os
import ntpath
import re
from pathlib import Path
import shlex
import subprocess
import sys
import zlib


def require(condition, message):
    if not condition:
        raise ValueError(message)


def transport_command(producer, command, mode=None, wsl=None):
    transport = producer['transport']
    if transport == 'wsl':
        require(sys.platform in ('win32', 'msys', 'cygwin') and mode in ('windows-import', 'wsl-native'),
                'direct WSL requires a native Windows physical route')
        require(isinstance(wsl, str) and ntpath.isabs(wsl) and ntpath.splitdrive(wsl)[0] and
                not any(c in wsl for c in '\0\r\n'), 'WSL executable is not an absolute native path')
        for name in ('distribution', 'user'):
            require(isinstance(producer.get(name), str) and
                    re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9._-]{0,127}', producer[name]),
                    'invalid WSL ' + name)
        require(not producer.get('_transport_admission'), 'direct WSL is inside the physical lease')
        return [wsl, '--distribution', producer['distribution'], '--user', producer['user'],
                '--exec', *command]
    require(transport in ('ssh', 'wsl-ssh'), 'producer requires native responder transport')
    prefix = []
    if transport == 'wsl-ssh':
        prefix = ['MSYS_NO_PATHCONV=1', 'wsl.exe', '--distribution', producer['distribution'],
                  '--user', producer['user'], '--exec']
    host = producer['host']
    require(host and not host.startswith('-') and all(c.isalnum() or c in '@._-' for c in host),
            'invalid SSH destination')
    return ['ssh', '-oBatchMode=yes', '--', host, shlex.join([*prefix, *command])]


class Remote:
    def __init__(self, producer, run, evidence, *, mode=None, wsl=None):
        self.run = run
        self.session = hashlib.sha256(run.encode()).hexdigest()[:16]
        self.seq = self.cmd = 0
        self.received = -1
        self.boot = '0' * 16
        state = producer['state_root'] + '/' + run
        command = [producer['etude'], 'serve', '--stdio', '-d', state]
        admission = producer.get('_transport_admission')
        if admission:
            command = ['/usr/bin/env', 'ETUDE_PREPARATION_GRANT=' + admission['grant'],
                producer['etude'], 'admit', '-i', '-d', producer['policy']['locks'],
                '-r', admission['result'], producer['policy']['ledger'], admission['claim'],
                producer['account'], 'shared', producer['preparation_resources'],
                producer['policy']['capacity'], str(admission['seconds']), '/usr/bin/env',
                'ETUDE_ADMITTED=' + producer['site'], 'ETUDE_SITE=' + producer['site'], *command]
        if admission:
            command = [producer['etude'], 'authority', 'transport', producer['site'],
                producer['policy']['ledger'], producer['policy']['locks'],
                producer['policy']['capacity'], str(admission['seconds']), *command]
        command = transport_command(producer, command, mode, wsl)
        environment = None
        if producer['transport'] == 'wsl':
            environment = dict(os.environ, MSYS2_ARG_CONV_EXCL='*')
        self.transcript = Path(evidence).open('xb')
        self.child = None
        try:
            self.child = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                          env=environment)
            self.handshake()
        except BaseException:
            if self.child is not None:
                self.abandon()
            else:
                self.transcript.close()
            raise
        self.files = {}

    def handshake(self):
        kind, fields = self.read()
        require(kind == 'alive' and len(fields) == 3 and '1' in fields[0].split(','),
                'responder did not offer frame version1')
        cmd = self.command('hello', '1', '65536')
        kind, fields = self.answer(cmd)
        require(kind == 'ready' and fields[1:3] == ['1', '65536'], 'responder handshake failed')

    def send(self, kind, *fields):
        values = ['#E1', self.session, self.boot, str(self.seq),
                  '-' if self.received < 0 else str(self.received), kind, *map(str, fields)]
        require(all(value and all('!' <= c <= '~' for c in value) for value in values),
                'frame contains a nonprintable field')
        body = (' '.join(values) + ' ').encode('ascii')
        line = body + f'*{zlib.crc32(body):08x}\n'.encode()
        require(len(line) <= 65536, 'frame exceeds negotiated bound')
        self.transcript.write(b'> ' + line)
        self.transcript.flush()
        self.child.stdin.write(line)
        self.child.stdin.flush()
        self.seq += 1

    def read(self):
        line = self.child.stdout.readline(65537)
        require(line and len(line) <= 65536 and line.endswith(b'\n'), 'responder connection lost')
        self.transcript.write(b'< ' + line)
        self.transcript.flush()
        body, crc = line[:-1].rsplit(b'*', 1)
        require(len(crc) == 8 and crc.decode() == f'{zlib.crc32(body):08x}', 'invalid responder CRC')
        words = body.decode('ascii').strip().split(' ')
        require(len(words) >= 6 and words[0] == '#E1' and int(words[3]) == self.received + 1,
                'responder frame sequence differs')
        require(words[4] == '-' or 0 <= int(words[4]) < self.seq, 'responder ACK exceeds sent frames')
        if self.received < 0:
            require(words[1] == '0' * 16 and words[5] == 'alive', 'invalid first frame')
            self.boot = words[2]
        else:
            require(words[1] == self.session and words[2] == self.boot, 'responder identity changed')
        self.received += 1
        return words[5], words[6:]

    def command(self, kind, *fields):
        value = str(self.cmd)
        self.cmd += 1
        self.send(kind, value, *fields)
        return value

    def answer(self, cmd):
        kind, fields = self.read()
        require(fields and fields[0] == cmd and kind != 'error', f'responder refused {cmd}: {kind} {fields}')
        return kind, fields

    def put(self, name, data, mode='0600'):
        require(name and '/' not in name and name not in ('.', '..'), 'invalid staged filename')
        sha = hashlib.sha256(data).hexdigest()
        cmd = self.command('put', name, len(data), sha, mode)
        for offset in range(0, len(data), 48000):
            self.send('data', cmd, offset, base64.b64encode(data[offset:offset + 48000]).decode())
        self.send('seal', cmd)
        kind, fields = self.answer(cmd)
        require(kind == 'stored' and fields == [cmd, name, sha], 'responder did not seal staged file')
        self.files[name] = dict(name=name, size_bytes=len(data), sha256=sha, mode=mode)
        return sha

    def execute(self, name, program, arguments, environment, output):
        attempt = self.run + ':' + name
        token = hashlib.sha256(attempt.encode()).hexdigest()[:16]
        invocation = dict(schema_version=1, run_id=self.run, attempt_id=attempt, attempt_token=token,
                          case_id='routine.' + name, program=program, arguments=arguments,
                          environment=environment, stdin=None, role='ordinary',
                          files=list(self.files.values()), fixtures=[dict(fixture='workspace', given=[])],
                          deadline_us=86400000000, grace_us=2000000, output_max_bytes=536870912)
        raw = json.dumps(invocation, sort_keys=True, separators=(',', ':')).encode()
        digest = self.put('invocation-' + name, raw)
        cmd = self.command('run', token, digest)
        streams = {fd: bytearray() for fd in ('1', '2')}
        seals = {}
        result = dict(attempt_id=attempt, attempt_token=token, responder_boot=self.boot)
        with Path(output).open('xb') as log:
            while True:
                kind, fields = self.answer(cmd)
                require(len(fields) >= 2 and fields[1] == token, 'foreign attempt event')
                if kind == 'started':
                    result['child'] = dict(pid=fields[2], creation=fields[3])
                elif kind == 'out':
                    fd, offset = fields[2], int(fields[3])
                    data = base64.b64decode(fields[4], validate=True)
                    require(offset == len(streams[fd]), 'noncontiguous responder output')
                    streams[fd].extend(data)
                    log.write(data)
                    log.flush()
                    os.write(1 if fd == '1' else 2, data)
                elif kind == 'exit':
                    result.update(end=fields[2], value=fields[3])
                elif kind == 'stream':
                    seals[fields[2]] = fields[3:6]
                elif kind == 'cleaned':
                    result['cleanup'] = fields[2]
                    break
                else:
                    require(kind == 'usage', 'unexpected responder event: ' + kind)
        for fd, data in streams.items():
            require(seals.get(fd) == [str(len(data)), str(len(data)), hashlib.sha256(data).hexdigest()],
                    'responder output was truncated or changed')
        Path(str(output) + '.result.json').write_text(json.dumps(result, indent=2) + '\n')
        require(result.get('child') and result.get('end') == 'exited' and result.get('value') == '0' and
                result.get('cleanup') == 'confirmed', 'remote preparation or cleanup failed')
        return token

    def get(self, token, name):
        cmd = self.command('get', token + '/work/' + name)
        data = bytearray()
        while True:
            kind, fields = self.answer(cmd)
            if kind == 'data':
                require(int(fields[1]) == len(data), 'noncontiguous returned proof')
                data.extend(base64.b64decode(fields[2], validate=True))
            else:
                require(kind == 'sent' and int(fields[2]) == len(data) and
                        fields[3] == hashlib.sha256(data).hexdigest(), 'returned proof digest differs')
                return bytes(data)

    def close(self):
        cmd = self.command('bye')
        kind, fields = self.answer(cmd)
        require(kind == 'bye' and fields == [cmd], 'responder did not confirm connection end')
        self.child.stdin.close()
        require(self.child.wait() == 0, 'responder transport cleanup failed')
        self.transcript.close()

    def abandon(self):
        # The native responder cancels its executor when its control input ends.
        # An uncertain transport never produces a successful phase proof.
        try:
            self.child.stdin.close()
        except OSError:
            pass
        finally:
            self.child.wait()
            self.transcript.close()
