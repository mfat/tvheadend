#!/usr/bin/env python3
"""Integration test against an isolated local server; no external services.

Usage: python3 support/test-sqlite-startup.py build.linux/tvheadend [channel-count]
Requires Linux, cc and sqlite3 development files (for a test-only interposer).
All configuration, ports, recordings and subprocesses belong to this test.
"""
from contextlib import closing
import importlib.util
import json
import os
from pathlib import Path
import socket
import sqlite3
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
NETWORK = '10000000000000000000000000000001'
CHANNEL = '20000000000000000000000000000001'
AUTOREC = '30000000000000000000000000000001'
RECORDING = '40000000000000000000000000000001'


def varint(value):
    parts = [value & 127]
    while value >> 7:
        value >>= 7
        parts.insert(0, (value & 127) | 128)
    return bytes(parts)


def encode(message, version=1):
    result = b''
    items = message.items() if isinstance(message, dict) else [('', v) for v in message]
    for key, value in items:
        if isinstance(value, str):
            kind, data = 3, value.encode()
        elif isinstance(value, (dict, list)):
            kind, data = (1 if isinstance(value, dict) else 5), encode(value, version)
        else:
            kind, data = 2, int(value).to_bytes((int(value).bit_length() + 7) // 8, 'little')
        key = key.encode()
        length = len(data).to_bytes(4, 'big') if version == 1 else varint(len(data))
        result += bytes([kind, len(key)]) + length + key + data
    return result


def decode(data, is_list=False):
    result = [] if is_list else {}
    while data:
        kind, namesize = data[:2]
        size = int.from_bytes(data[2:6], 'big')
        name = data[6:6 + namesize].decode()
        value = data[6 + namesize:6 + namesize + size]
        data = data[6 + namesize + size:]
        if kind in (1, 5): value = decode(value, kind == 5)
        elif kind == 3: value = value.decode()
        elif kind in (2, 7): value = int.from_bytes(value, 'little')
        if is_list: result.append(value)
        else: result[name] = value
    return result


def receive(sock):
    def read(size):
        chunks = b''
        while len(chunks) < size:
            part = sock.recv(size - len(chunks))
            if not part: raise RuntimeError('HTSP closed')
            chunks += part
        return chunks
    return decode(read(int.from_bytes(read(4), 'big')))


def send(sock, message):
    payload = encode(message)
    sock.sendall(len(payload).to_bytes(4, 'big') + payload)


def wait_for(check, timeout=30):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        value = check()
        if value: return value
        time.sleep(.02)
    raise AssertionError('timed out waiting for fixture condition')


def port():
    with socket.socket() as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]


def fixture(config, count):
    spec = importlib.util.spec_from_file_location('importer', ROOT / 'support/settings-sqlite-import.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    database = config / 'settings.sqlite'
    with closing(sqlite3.connect(database)) as db:
        db.executescript(module.SCHEMA)
        ordinal = 0
        def put(path, data=None):
            nonlocal ordinal
            if db.execute('SELECT 1 FROM nodes WHERE path=?', (path,)).fetchone(): return
            parent, _, name = path.rpartition('/')
            if parent: put(parent)
            db.execute('INSERT INTO nodes VALUES(?,?,?,?,?,?)',
                       (path, parent, name, int(data is None), ordinal,
                        None if data is None else json.dumps(data).encode()))
            ordinal += 1
        put(f'input/iptv/networks/{NETWORK}/config',
            {'networkname': 'Fixture', 'class': 'iptv_network', 'enabled': False, 'skipinitscan': True})
        for i in range(count):
            # Channel IDs use the first four UUID bytes (31 bits).
            channel = f'{int(CHANNEL, 16) + (i << 104):032x}'
            mux = f'{0x50000000000000000000000000000001 + i:032x}'
            service = f'{0x60000000000000000000000000000001 + i:032x}'
            put(f'channel/config/{channel}', {'enabled': True, 'autoname': True, 'services': [service]})
            prefix = f'input/iptv/networks/{NETWORK}/muxes/{mux}'
            put(prefix + '/config', {'enabled': False, 'iptv_url': 'udp://127.0.0.1:9', 'scan_state': 0})
            put(prefix + '/services/' + service, {'sid': 1, 'svcname': f'Fixture {i}', 'svc_type': 1})
        start = int(time.time()) + 3600
        put('dvr/config/70000000000000000000000000000001', {'name': '', 'storage': str(config / 'recordings')})
        put(f'dvr/autorec/{AUTOREC}', {'enabled': True, 'title': 'Regression', 'channel': CHANNEL})
        put(f'dvr/log/{RECORDING}', {'enabled': True, 'channel': CHANNEL, 'broadcast': 1000,
            'autorec': AUTOREC, 'start': start, 'stop': start + 3600,
            'title': {'eng': 'Regression'}, 'comment': 'preserve this customization'})
        db.commit()
    messages = [{'__section__': 'config'}, {'last_id': 1001}, {'__section__': 'broadcasts'},
                {'id': 1000, 'tp': 1, 'gr': 'eit', 'ch': CHANNEL, 'start': start, 'stop': start + 3600,
                 'tit': {'eng': 'Regression'}}]
    payloads = [encode(m, 2) for m in messages]
    (config / 'epgdb.v3').write_bytes(b''.join(varint(len(p)) + p for p in payloads))
    return database


class Server:
    def __init__(self, binary, config, preload=None, gate=None):
        self.http, self.htsp = port(), port()
        env = os.environ.copy()
        env['TVH_SETTINGS_SQLITE'] = str(config / 'settings.sqlite')
        if preload:
            env['LD_PRELOAD'] = str(preload)
            env['TVH_TEST_STARTUP_GATE'] = str(gate)
        self.log = open(config / 'server.log', 'ab')
        self.process = subprocess.Popen([str(binary), '-c', str(config), '--noacl', '-B', '-S',
            '-b', '127.0.0.1', '--http_port', str(self.http), '--htsp_port', str(self.htsp),
            '--nosatipcli', '--satip_rtsp', '-1', '-a', '-1'], env=env,
            stdout=self.log, stderr=self.log)

    def api(self, path, **args):
        url = f'http://127.0.0.1:{self.http}/api/{path}'
        request = urllib.request.Request(url, urllib.parse.urlencode(args).encode())
        try:
            with urllib.request.urlopen(request, timeout=3) as response:
                return response.status, json.load(response)
        except urllib.error.HTTPError as exc:
            exc.close()
            return exc.code, None

    def ready(self):
        assert self.process.poll() is None, f'server exited: {self.process.returncode}'
        try:
            status, response = self.api('serverinfo')
            return status == 200 and response['inputs_loaded']
        except (OSError, urllib.error.URLError):
            return False

    def stop(self):
        self.process.terminate()
        # Large fixtures flush and tear down tens of thousands of muxes.
        try: self.process.wait(timeout=60)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait(timeout=5)
            raise
        finally: self.log.close()


def main():
    binary = Path(sys.argv[1]).resolve()
    count = int(sys.argv[2]) if len(sys.argv) > 2 else 1024
    with tempfile.TemporaryDirectory(prefix='tvh-sqlite-integration-') as tmp:
        config = Path(tmp)
        database = fixture(config, count)
        preload = config / 'pause.so'
        subprocess.run(['cc', '-shared', '-fPIC', str(ROOT / 'support/test-startup-pause.c'),
                        '-ldl', '-lsqlite3', '-o', str(preload)], check=True, timeout=30)
        gate = config / 'gate'
        gate.touch()
        server = Server(binary, config, preload, gate)
        try:
            wait_for(lambda: gate.with_suffix('.entered').exists())
            assert server.api('serverinfo')[1]['inputs_loaded'] is False
            assert server.api('channel/grid', limit=1)[0] == 200
            for _ in range(3):
                assert server.api('idnode/delete', uuid=NETWORK)[0] == 503
            assert server.api('dvr/entry/grid')[0] == 503
            with socket.create_connection(('127.0.0.1', server.htsp), timeout=10) as sock:
                send(sock, {'method': 'hello', 'htspversion': 43, 'clientname': 'isolated regression'})
                receive(sock)
                send(sock, {'method': 'api', 'path': 'idnode/delete', 'args': {'uuid': NETWORK}})
                assert 'error' in receive(sock)
                # A second request verifies the error path reacquired global_lock.
                send(sock, {'method': 'enableAsyncMetadata'})
                initial = None
                while True:
                    msg = receive(sock)
                    if msg.get('channelIdStr') == CHANNEL: initial = msg
                    if msg.get('method') == 'initialSyncCompleted': break
                assert initial is not None and not initial['services']
                gate.unlink()
                started = time.monotonic()
                updated = None
                deadline = started + 120
                sock.settimeout(120)
                while time.monotonic() < deadline:
                    msg = receive(sock)
                    if msg.get('channelIdStr') == CHANNEL and msg.get('services'):
                        updated = msg
                        break
                assert updated is not None, 'early HTSP client never received completed metadata'
                assert updated['channelName'] == 'Fixture 0', updated
                wait_for(server.ready, 120)
                print(f'{count} channels: ready after gate release in {time.monotonic()-started:.3f}s')
            entries = server.api('dvr/entry/grid')[1]['entries']
            entry = next((e for e in entries if e['uuid'] == RECORDING), None)
            assert entry is not None, 'saved autorecording was purged during startup'
            assert entry['comment'] == 'preserve this customization'
            assert entry['broadcast'] == 1000
            status, _ = server.api('idnode/save', node=json.dumps({'uuid': CHANNEL, 'name': 'Saved name', 'autoname': False}))
            assert status == 200
            # Exercise packed mux serialization as well as channel JSON.
            mux = '50000000000000000000000000000001'
            assert server.api('idnode/save', node=json.dumps({'uuid': mux, 'iptv_muxname': 'Saved mux'}))[0] == 200
        finally:
            gate.unlink(missing_ok=True)
            server.stop()
            if sys.exc_info()[0]:
                log = (config / 'server.log').read_text(errors='replace')
                print(log[:12000] + '\n...\n' + log[-2000:], file=sys.stderr)
        with closing(sqlite3.connect(database)) as db:
            assert db.execute('PRAGMA integrity_check').fetchone()[0] == 'ok'
            raw = db.execute('SELECT data FROM nodes WHERE path=?',
                (f'input/iptv/networks/{NETWORK}/muxes/{mux}',)).fetchone()[0]
            assert raw.startswith(b'\xff\xffGZIP01'), 'mux was not saved using portable packed serialization'
        server = Server(binary, config)
        try:
            wait_for(server.ready, 120)
            channels = server.api('channel/grid', limit=count)[1]['entries']
            assert next(c for c in channels if c['uuid'] == CHANNEL)['name'] == 'Saved name'
            # Check that mutations resume after startup. Keep the large run
            # focused on startup/persistence rather than bulk tuner deletion.
            assert server.api('idnode/delete', uuid=NETWORK if count <= 1024 else CHANNEL)[0] == 200
        finally:
            server.stop()
        print('startup guard, HTSP update, DVR preservation and SQLite save/restart: passed')


if __name__ == '__main__':
    main()
