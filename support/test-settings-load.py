#!/usr/bin/env python3
"""Test the built server's settings loader with GDB, without starting a server.

Usage: python3 support/test-settings-load.py [path/to/tvheadend]
Requires a debug-symbol build with file bundles and zlib enabled, and GDB.
"""
import gzip
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

binary = Path(sys.argv[1] if len(sys.argv) > 1 else
              Path(__file__).resolve().parents[1] / 'build.linux/tvheadend')
with tempfile.TemporaryDirectory(prefix='tvh-settings-test-') as directory:
    root = Path(directory)
    records = root / 'records'
    (records / 'nested').mkdir(parents=True)
    (root / 'empty').mkdir()
    (records / 'plain').write_text('{"value": 7}')
    (records / 'nested' / 'child').write_text('{"value": 9}')
    (records / '.hidden').write_text('{"value": 0}')
    (records / 'backup~').write_text('{"value": 0}')
    (records / 'invalid').write_text('invalid JSON')
    (records / 'linked').symlink_to('plain')
    # HTSMSG binary v1: one integer field named "value", value 11.
    payload = bytes([2, 5, 0, 0, 0, 1]) + b'value' + bytes([11])
    (records / 'compressed').write_bytes(
        b'\xff\xffGZIP00' + len(payload).to_bytes(4, 'big') + gzip.compress(payload))
    # Enough entries to load on several threads: every kind of entry, in
    # the directory's own (unsorted) order.
    many = root / 'many'
    many.mkdir()
    for i in range(600):
        name = f'entry{i:03d}'
        kind = i % 7
        if kind == 0:
            (many / name).write_text(json.dumps({'value': i}))
        elif kind == 1:
            data = bytes([2, 5, 0, 0, 0, 1]) + b'value' + bytes([i % 256])
            (many / name).write_bytes(
                b'\xff\xffGZIP00' + len(data).to_bytes(4, 'big') + gzip.compress(data))
        elif kind == 2:
            (many / name).write_text('invalid JSON')
        elif kind == 3:
            (many / ('.' + name)).write_text('{"value": 0}')
        elif kind == 4:
            (many / (name + '~')).write_text('{"value": 0}')
        elif kind == 5:
            (many / name).mkdir()
            (many / name / 'child').write_text(json.dumps({'value': i}))
        else:
            (many / name).symlink_to(f'entry{i - 6:03d}')
    def many_expected(recursive):
        result = {}
        for name in os.listdir(many):
            if name.startswith('.') or name.endswith('~'):
                continue
            i = int(name[5:])
            kind = i % 7
            if kind == 0 or kind == 6:
                result[name] = {'value': i if kind == 0 else i - 6}
            elif kind == 1:
                result[name] = {'value': i % 256}
            elif kind == 5 and recursive:
                result[name] = {'child': {'value': i}}
        return result
    commands = '''set pagination off
set confirm off
set debuginfod enabled off
start
python
import gdb, json
root = ROOT
# Invoke the real loader in the built executable; main is never continued.
gdb.execute('call hts_settings_init(%s)' % json.dumps(root), to_string=True)
def load(path, recursive=False, bundled=False):
    expr = ('hts_settings_load_r(1, %s)' if recursive else 'hts_settings_load(%s)') % json.dumps(path)
    if bundled:
        expr = 'hts_settings_load_path(%s, %d)' % (json.dumps(path), int(recursive))
    message = gdb.parse_and_eval(expr)
    if not int(message):
        return None
    address = int(message)
    text = gdb.parse_and_eval('htsmsg_json_serialize_to_str((htsmsg_t *)%d, 0)' % address)
    result = json.loads(text.string())
    gdb.execute('call free((void *)%d)' % int(text), to_string=True)
    gdb.execute('call htsmsg_destroy((htsmsg_t *)%d)' % address, to_string=True)
    return result
expected = {'plain': {'value': 7}, 'linked': {'value': 7}, 'compressed': {'value': 11}}
assert load('records') == expected
assert load('records', True) == dict(expected, nested={'child': {'value': 9}})
assert load('records/plain') == {'value': 7}
assert load('records/compressed') == {'value': 11}
assert load('empty') == {}
assert load('missing-settings-test') is None
# A missing local file/directory must still fall back to packaged defaults.
assert load('profiles') == load('data/conf/profiles', bundled=True)
assert load('profiles')
assert load('epggrab/eit/scrape') == load('data/conf/epggrab/eit/scrape', bundled=True)
assert load('epggrab/eit/scrape')
for recursive, want in ((False, MANY_FLAT), (True, MANY_DEEP)):
    got = load('many', recursive)
    assert got == want, 'parallel load differs'
    assert list(got) == list(want), 'parallel load changed the order'
print('PASS: plain, gzip, recursive, symlink, hidden/backup exclusion, missing and bundled settings; '
      'a 600-entry directory loaded on several threads in directory order')
end
kill
quit
'''.replace('root = ROOT', 'root = ' + repr(str(root))
          ).replace('MANY_FLAT', repr(many_expected(False))
          ).replace('MANY_DEEP', repr(many_expected(True)))
    script = root / 'test.gdb'
    script.write_text(commands)
    subprocess.run(['gdb', '-q', '-batch', '-x', str(script), '--args',
                    str(binary.resolve())], check=True)
