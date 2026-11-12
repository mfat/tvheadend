#!/usr/bin/env python3
"""Test the built server's settings loader with GDB, without starting a server.

Usage: python3 support/test-settings-load.py [path/to/tvheadend]
Requires a debug-symbol build with file bundles and zlib enabled, and GDB.
"""
import gzip
import json
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
print('PASS: plain, gzip, recursive, symlink, hidden/backup exclusion, missing and bundled settings')
end
kill
quit
'''.replace('root = ROOT', 'root = ' + repr(str(root)))
    script = root / 'test.gdb'
    script.write_text(commands)
    subprocess.run(['gdb', '-q', '-batch', '-x', str(script), '--args',
                    str(binary.resolve())], check=True)
