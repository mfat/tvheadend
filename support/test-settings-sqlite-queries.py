#!/usr/bin/env python3
"""Exercise the backend's actual subtree SQL with a large isolated database."""
from contextlib import closing
from pathlib import Path
import re
import sqlite3
import unittest


class SubtreeQueries(unittest.TestCase):
    def test_replacement_and_removal_are_indexed_and_preserve_siblings(self):
        source = Path(__file__).resolve().parents[1] / 'src/settings_sqlite.c'
        statements = re.findall(r'"DELETE FROM nodes[^;]+?-1, &st, NULL', source.read_text())
        queries = [''.join(re.findall(r'"([^"\n]*)"', statement)) for statement in statements]
        queries = [query for query in queries if 'substr(' in query or "|| '/'" in query]
        self.assertEqual(len(queries), 2)
        for query in queries:
            with self.subTest(query=query), closing(sqlite3.connect(':memory:')) as db:
                db.execute('CREATE TABLE nodes(path TEXT PRIMARY KEY)')
                db.executemany('INSERT INTO nodes VALUES(?)',
                               ((f'unrelated/{i}',) for i in range(60000)))
                target = 'muxes/Case_%é'
                children = {target + '/config', target + '/services/one'}
                siblings = {target + '0', target + '-old/config', 'muxes/case_%é/config',
                            target + 'extra/config', 'muxes/Case_XXé/config'}
                db.executemany('INSERT INTO nodes VALUES(?)',
                               ((path,) for path in children | siblings | {target}))
                db.commit()
                ticks = 0
                def progress():
                    nonlocal ticks
                    ticks += 1
                    return ticks > 100  # At most 10,000 VM instructions.
                db.set_progress_handler(progress, 100)
                try:
                    db.execute(query, (target,))
                finally:
                    db.set_progress_handler(None, 0)
                remaining = {row[0] for row in db.execute('SELECT path FROM nodes')}
                self.assertFalse(children & remaining)
                self.assertTrue(siblings <= remaining)
                self.assertEqual(target in remaining, 'WHERE path=?1 OR' not in query)
                self.assertEqual(sum(p.startswith('unrelated/') for p in remaining), 60000)
                db.rollback()
                self.assertTrue(children <= {r[0] for r in db.execute('SELECT path FROM nodes')})


if __name__ == '__main__':
    unittest.main()
