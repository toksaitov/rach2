"""Keep eight independent processor loads active inside a reference guest."""
import hashlib
import multiprocessing
import signal
import time


def burn(counter):
    """Count completed CPU work until the parent stops this worker."""
    while True:
        hashlib.pbkdf2_hmac('sha256', b'browser-load', b'rach2', 100000)
        with counter.get_lock():
            counter.value += 1


def main():
    """Publish readiness after every worker advances; reap workers on every exit."""
    children = []
    counters = [multiprocessing.Value('Q', 0) for _ in range(8)]
    for sig in (signal.SIGTERM, signal.SIGHUP, signal.SIGINT):
        signal.signal(sig, lambda *_: exit(0))
    try:
        for counter in counters:
            child = multiprocessing.Process(target=burn, args=(counter,))
            child.start()
            children.append(child)
        ready = False
        while True:
            assert all(child.is_alive() for child in children), 'load worker exited'
            values = [counter.value for counter in counters]
            if not ready and min(values) > 0:
                print('browser-load: ready: 8 processors', flush=True)
                ready = True
            print('browser-load: progress: ' + ' '.join(map(str, values)), flush=True)
            time.sleep(1)  # Poll the workers' completed-work counters.
    finally:
        for child in children:
            child.terminate()
        for child in children:
            child.join()


if __name__ == '__main__':
    main()
