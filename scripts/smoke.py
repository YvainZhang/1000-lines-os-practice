#!/usr/bin/env python3
"""Build and exercise QEMU serial console; Python standard library only."""
import argparse
import os
from pathlib import Path
import re
import selectors
import signal
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]


def run(ncpu, timeout):
    logdir = ROOT / 'test-results'
    logdir.mkdir(exist_ok=True)
    path = logdir / f'smoke-{ncpu}.log'
    env = dict(os.environ, NCPU=str(ncpu))
    with path.open('wb') as log:
        proc = subprocess.Popen(['bash', 'run.sh'], cwd=ROOT, env=env,
                                stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, start_new_session=True)
        selector = selectors.DefaultSelector()
        selector.register(proc.stdout, selectors.EVENT_READ)
        pending = b''

        def expect(marker):
            nonlocal pending
            deadline = time.monotonic() + timeout
            while True:
                failure = re.search(
                    rb'(?:SELFTEST FAIL|PANIC|unexpected trap)[^\r\n]*[\r\n]',
                    pending, re.I)
                if failure:
                    raise RuntimeError(failure.group().decode(errors='replace').strip())
                if marker in pending:
                    pending = pending.split(marker, 1)[1]
                    return
                if time.monotonic() >= deadline:
                    raise RuntimeError(f'timeout waiting for {marker!r}')
                if not selector.select(timeout=0.2):
                    continue
                chunk = os.read(proc.stdout.fileno(), 65536)
                if not chunk:
                    raise RuntimeError(f'process ended before {marker!r}')
                log.write(chunk)
                log.flush()
                pending += chunk

        def command(value):
            proc.stdin.write(value.encode() + b'\r')
            proc.stdin.flush()

        try:
            expect(f'smp: {ncpu} hart(s) online'.encode())
            expect(b'> ')
            command('help')
            expect(b'Show commands')
            expect(b'> ')
            command('hello')
            expect(b'Hello world from shell!')
            expect(b'> ')
            command('selftest')
            expect(b'SELFTEST PASS')
            expect(b'> ')
            command('irqstat')
            expect(f'ncpu={ncpu}'.encode())
            expect(b'context-switch=')
            expect(b'uart-rx-dropped=0')
            expect(b'> ')
            command('exit')
            # Exit ends the shell, not QEMU. Its echo only confirms input receipt.
            expect(b'exit\r')
            proc.stdin.write(b'\x01x')
            proc.stdin.flush()
            proc.wait(timeout=5)
            if proc.returncode != 0:
                raise RuntimeError(f'QEMU exit status {proc.returncode}')
        finally:
            if proc.poll() is None:
                os.killpg(proc.pid, signal.SIGTERM)
                try:
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    os.killpg(proc.pid, signal.SIGKILL)
                    proc.wait()
            selector.close()
            proc.stdin.close()
            proc.stdout.close()
    print(f'PASS NCPU={ncpu}: {path.relative_to(ROOT)}', flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cpus', type=int, nargs='+', choices=range(1, 5),
                        default=[1, 2, 4])
    parser.add_argument('--timeout', type=float, default=90,
                        help='seconds allowed for each expected output')
    args = parser.parse_args()
    if args.timeout <= 0:
        parser.error('--timeout must be positive')
    for ncpu in args.cpus:
        try:
            run(ncpu, args.timeout)
        except (RuntimeError, OSError, subprocess.TimeoutExpired) as error:
            parser.exit(1, f'FAIL NCPU={ncpu}: {error}; see test-results/smoke-{ncpu}.log\n')


if __name__ == '__main__':
    main()
