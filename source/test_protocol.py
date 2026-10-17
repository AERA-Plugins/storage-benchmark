"""Exercise native pages and cancellation against a strict API-2 socket host."""
import os
from pathlib import Path
import select
import signal
import socket
import struct
import tempfile
import time

WIRE = struct.Struct('<6I96s1024s')
MAGIC = 0x41325049
with tempfile.TemporaryDirectory(prefix='aera-bench-protocol-', dir=Path.home()) as temp:
    root = Path(temp)
    (root / 'usr/bin').mkdir(parents=True)
    (root / 'usr/bin/fio').symlink_to('/usr/bin/fio')
    destination = root / 'storage'
    destination.mkdir()
    sentinel = destination / 'keep.txt'
    sentinel.write_bytes(b'unchanged')
    host, child = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    pid = os.fork()
    if pid == 0:
        host.close()
        os.dup2(child.fileno(), 4, inheritable=True)
        os.set_inheritable(4, True)
        os.environ.update(AERA_PLUGIN_ROOT=str(root), BENCH_FIXTURE=str(destination), BENCH_SETTINGS_DIR=str(root / 'settings'))
        binary = os.environ.get('BENCH_PLUGIN', '/tmp/aera-bench-ui-test')
        emulator = os.environ.get('BENCH_EMULATOR')
        if emulator:
            os.execv(emulator, [emulator, binary])
        os.execv(binary, [binary])
    child.close()
    pending = False
    rows, buttons, back, commits, updates = set(), {}, 0, 0, 0

    def send(kind, request=0, value=0, flags=0):
        host.send(WIRE.pack(MAGIC, 2, kind, request, value, flags, b'', b''))

    def receive(until_commit=True, timeout=8):
        global pending, rows, buttons, back, commits, updates
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            if not select.select([host], [], [], end - time.monotonic())[0]:
                break
            raw = host.recv(8192)
            assert len(raw) == WIRE.size, ('invalid packet length', len(raw))
            magic, version, kind, request, value, flags, title, text = WIRE.unpack(raw)
            assert magic == MAGIC and version == 2
            if kind == 1:
                send(64, flags=3)
                send(66, value=1)
            elif kind == 2:
                assert not pending
                pending, rows, buttons, back = True, set(), {}, 0
            elif kind == 3:
                assert pending and request not in buttons
                buttons[request] = flags
                assert len(buttons) <= 12
            elif kind == 8:
                assert pending and title.strip(b'\0')
            elif kind == 9:
                assert pending and request and request not in rows
                rows.add(request)
                assert len(rows) <= 64
            elif kind == 10:
                assert not pending and request in rows
                updates += 1
                if not until_commit:
                    return
            elif kind == 11:
                assert pending and request
                back = request
            elif kind == 4:
                assert pending and (not back or back in buttons and not buttons[back] & 4)
                pending = False
                commits += 1
                print('PAGE:', title.split(b'\0')[0], 'metrics', len(rows), 'buttons', len(buttons))
                return
        raise AssertionError('Host API deadline exceeded')

    try:
        receive()
        for action in (101, 120, 100, 102, 124, 125, 126, 127, 128, 129, 130, 100, 103, 304, 305, 100, 104, 100, 106, 100, 104, 121):
            send(65, action)
            receive()
        receive(until_commit=False)
        send(65, 107)
        receive()
        send(65, 123)
        receive()
        send(65, 107)
        receive()
        send(65, 122)
        receive()
        receive(timeout=10)
        send(66, value=2)
        send(66, value=1)
        receive()
        send(66, value=3)
        _, status = os.waitpid(pid, 0)
        pid = None
        assert os.waitstatus_to_exitcode(status) == 0
        assert sentinel.read_bytes() == b'unchanged'
        assert list(destination.iterdir()) == [sentinel]
        print('PASS:', commits, 'valid pages,', updates, 'live updates, cancel, resume, cleanup')
    finally:
        host.close()
        if pid is not None:
            os.kill(pid, signal.SIGTERM)
            os.waitpid(pid, 0)
