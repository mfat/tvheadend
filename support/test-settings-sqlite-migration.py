#!/usr/bin/env python3
"""Hermetic migration regressions; no server, downloads, or user configuration."""
import gzip
from contextlib import closing
import importlib.util
import os
from pathlib import Path
import sqlite3
import sys
import tempfile
import unittest
from unittest.mock import patch

SCRIPT = Path(__file__).with_name('settings-sqlite-import.py')
spec = importlib.util.spec_from_file_location('settings_import', SCRIPT)
importer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(importer)


class MigrationTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.config = self.root / 'config'
        self.config.mkdir()
        self.output = self.config / 'settings.sqlite.new'

    def run_import(self, output=None):
        args = [str(SCRIPT), str(self.config)]
        if output is not None:
            args += ['-o', str(output)]
        with patch.object(sys, 'argv', args):
            importer.main()

    def rows(self, output=None):
        with closing(sqlite3.connect(output or self.output)) as db:
            return dict(db.execute('SELECT path, data FROM nodes WHERE kind=0'))

    def link(self, source, dest, directory=False):
        try:
            dest.symlink_to(source, target_is_directory=directory)
        except OSError as exc:
            self.skipTest(f'symlinks unavailable: {exc}')

    def test_bytes_order_exclusions_and_no_self_import(self):
        records = self.config / 'channel'
        records.mkdir()
        (records / 'two').write_bytes(b'{"name":"Two"}')
        packed = b'\xff\xffGZIP01' + (12).to_bytes(4, 'big') + gzip.compress(b'packed bytes')
        (records / 'one').write_bytes(packed)
        (records / '.hidden').write_text('skip')
        (records / 'backup~').write_text('skip')
        (self.config / 'epgdb.v3').write_text('skip')
        (self.config / 'imagecache' / 'data').mkdir(parents=True)
        (self.config / 'imagecache' / 'data' / '1').write_text('skip')
        self.run_import()
        self.assertEqual(self.rows(), {'channel/two': b'{"name":"Two"}', 'channel/one': packed})
        with closing(sqlite3.connect(self.output)) as db:
            order = [r[0] for r in db.execute("SELECT name FROM nodes WHERE parent='channel' ORDER BY ordinal")]
        self.assertEqual(order, [p.name for p in records.iterdir() if p.name in ('one', 'two')])

    def test_custom_output_inside_source_is_not_imported(self):
        output = self.config / 'custom.db'
        (self.config / 'config').write_text('{}')
        self.run_import(output)
        self.assertEqual(self.rows(output), {'config': b'{}'})

    def test_file_symlink_preserves_target_bytes(self):
        target = self.root / 'source'
        target.write_text('{"name":"Linked"}')
        self.link(target, self.config / 'channel')
        self.run_import()
        self.assertEqual(self.rows()['channel'], target.read_bytes())

    def test_directory_symlink_preserves_descendants(self):
        target = self.root / 'source'
        target.mkdir()
        (target / 'one').write_text('{}')
        self.link(target, self.config / 'channel', True)
        self.run_import()
        self.assertEqual(self.rows()['channel/one'], b'{}')

    def test_dangling_link_fails_without_leaving_database(self):
        self.link(self.root / 'missing', self.config / 'channel')
        with self.assertRaises((OSError, RuntimeError)):
            self.run_import()
        self.assertFalse(self.output.exists())

    def test_directory_cycle_fails_without_leaving_database(self):
        self.link(self.config, self.config / 'loop', True)
        with self.assertRaisesRegex(RuntimeError, 'cycle'):
            self.run_import()
        self.assertFalse(self.output.exists())

    def test_repeated_import_never_overwrites_existing_output(self):
        self.run_import()
        before = self.output.read_bytes()
        with self.assertRaises((SystemExit, FileExistsError)):
            self.run_import()
        self.assertEqual(self.output.read_bytes(), before)

    def test_unreadable_source_rolls_back(self):
        (self.config / 'channel').write_text('{}')
        with patch('builtins.open', side_effect=PermissionError('fixture denied')):
            with self.assertRaises(RuntimeError):
                self.run_import()
        self.assertFalse(self.output.exists())

    def test_interrupt_removes_partial_output(self):
        with patch.object(importer, 'import_tree', side_effect=KeyboardInterrupt):
            with self.assertRaises(KeyboardInterrupt):
                self.run_import()
        self.assertFalse(self.output.exists())

    def test_missing_source_does_not_publish_database(self):
        self.config.rmdir()
        with self.assertRaises((OSError, RuntimeError)):
            self.run_import(self.root / 'output.db')
        self.assertFalse((self.root / 'output.db').exists())


if __name__ == '__main__':
    unittest.main()
