# Read the VM's counters when its inotify watcher sees the load markers.
import json
import socket
import subprocess
import threading
import time


class ExactLoad:
    """Own one guest watcher, reader thread and the two QMP snapshots."""
    def __init__(self, cmd, ssh, wrap, work, out, tag):
        self.out = out
        self.socket = work / ('qmp-' + tag + '.sock')
        self.ready = threading.Event()
        self.started = False
        self.error = (out / 'exact-stderr.txt').open('w')
        cmd('cat > /tmp/task4801-loadwatch.py', (work / 'loadwatch.py').read_bytes())
        self.process = subprocess.Popen(ssh + [wrap('python3 -u /tmp/task4801-loadwatch.py')],
                                        stdout=subprocess.PIPE, stderr=self.error, text=True)
        self.reader = threading.Thread(target=self.read, daemon=True)
        self.reader.start()
        if not self.ready.wait(15) or not self.started:
            self.close()
            raise RuntimeError('exact load watcher did not become ready')

    def read(self):
        """Capture each event through this VM's own QMP socket."""
        try:
            for line in self.process.stdout:
                if line.strip() == 'READY':
                    self.started = True
                    self.ready.set()
                    continue
                event = json.loads(line)
                with socket.socket(socket.AF_UNIX) as sk:
                    sk.settimeout(3)
                    sk.connect(str(self.socket))
                    with sk.makefile('rw') as stream:
                        stream.readline()

                        def ask(command, **arguments):
                            """Return one reply, skipping asynchronous QMP events."""
                            stream.write(json.dumps({'execute': command, 'arguments': arguments}) + '\n')
                            stream.flush()
                            while True:
                                reply = json.loads(stream.readline())
                                if 'return' in reply or 'error' in reply:
                                    return reply

                        ask('qmp_capabilities')
                        before = time.monotonic_ns()
                        reply = ask('query-stats', target='vcpu', providers=[{'provider': 'kvm'}])
                        event['host_before_ns'] = before
                        event['host_after_ns'] = time.monotonic_ns()
                reply['measurement'] = event
                (self.out / ('exact-' + event['stage'] + '.json')).write_text(json.dumps(reply, indent=2) + '\n')
        except Exception as error:
            (self.out / 'exact-error.txt').write_text(repr(error) + '\n')
        finally:
            self.ready.set()

    def close(self):
        """Reap the watcher and finish its reader on every harness exit."""
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
        else:
            self.process.wait()
        self.reader.join(timeout=5)
        if self.reader.is_alive():
            raise RuntimeError('exact load reader did not finish')
        self.process.stdout.close()
        self.error.close()
