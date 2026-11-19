#!/usr/bin/env python3
"""Copy a Tvheadend settings directory into settings.sqlite.

One row per file, in directory order. The bytes are copied unchanged, so
gzip-packed mux files stay packed. The programme database (epgdb) and
image-cache pictures are left as files because Tvheadend opens those
directly. Run this only while Tvheadend is stopped, and only against a
copy of the settings directory until the result has been checked.
"""

import argparse
import os
import sqlite3
import stat
import sys

SCHEMA = """
CREATE TABLE nodes (
  path TEXT PRIMARY KEY,
  parent TEXT,
  name TEXT NOT NULL,
  kind INTEGER NOT NULL,
  ordinal INTEGER NOT NULL,
  data BLOB
);
CREATE INDEX nodes_children ON nodes(parent, ordinal);
INSERT INTO nodes(path, parent, name, kind, ordinal, data)
VALUES('', NULL, '', 1, 0, NULL);
"""


def skip_name(name):
    return not name or name[0] == "." or name[-1] == "~"


def excluded(rel):
    base = rel.split("/")[-1]
    if base == "settings.sqlite" or base.startswith("settings.sqlite-"):
        return True
    if base == "epgdb" or base.startswith("epgdb.v"):
        return True
    if rel == "imagecache/data" or rel.startswith("imagecache/data/"):
        return True
    return False


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("config", help="Tvheadend settings directory")
    parser.add_argument(
        "-o",
        "--output",
        help="database to write (default: <config>/settings.sqlite.new)",
    )
    args = parser.parse_args()
    config = os.path.abspath(args.config)
    output = args.output or os.path.join(config, "settings.sqlite.new")
    if os.path.exists(output):
        sys.exit(f"{output} already exists")

    # Claim the output atomically; never truncate another import's file.
    fd = os.open(output, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    os.close(fd)
    db = None
    try:
        db = sqlite3.connect(output)
        db.executescript(SCHEMA)
        counts = import_tree(db, config, output)
        check = db.execute("PRAGMA integrity_check").fetchone()[0]
        if check != "ok":
            raise RuntimeError(f"integrity check failed: {check}")
        db.commit()
    except BaseException:
        # Cancellation must not leave an apparently usable partial database.
        if db is not None:
            db.close()
        os.remove(output)
        raise
    db.close()
    print(
        f"wrote {output}: {counts[0]} directories, {counts[1]} files",
        file=sys.stderr,
    )


def import_tree(db, config, output=None):
    directories = 0
    files = 0
    active_dirs = set()
    output_paths = set()
    if output is not None:
        output_paths = {os.path.realpath(output + suffix)
                        for suffix in ('', '-journal', '-wal', '-shm')}

    def ensure_dir(rel):
        nonlocal directories
        if not rel:
            return
        parent, _, name = rel.rpartition("/")
        row = db.execute("SELECT kind FROM nodes WHERE path=?", (rel,)).fetchone()
        if row:
            if row[0] != 1:
                raise RuntimeError(f"{rel} is a file")
            return
        ensure_dir(parent)
        db.execute(
            "INSERT INTO nodes(path, parent, name, kind, ordinal, data)"
            " VALUES(?, ?, ?, 1,"
            " COALESCE((SELECT MAX(ordinal) FROM nodes WHERE parent=?), -1) + 1,"
            " NULL)",
            (rel, parent, name, parent),
        )
        directories += 1

    def walk(directory, rel):
        nonlocal files
        info = os.stat(directory)
        identity = (info.st_dev, info.st_ino)
        if identity in active_dirs:
            raise RuntimeError(f"directory symlink cycle at {directory}")
        active_dirs.add(identity)
        try:
            entries_cm = os.scandir(directory)
        except OSError as exc:
            raise RuntimeError(f"cannot list {directory}: {exc}") from exc
        with entries_cm as entries:
            ordered = list(entries)
        for entry in ordered:
            name = entry.name
            child_rel = f"{rel}/{name}" if rel else name
            if skip_name(name) or excluded(child_rel):
                continue
            if os.path.realpath(entry.path) in output_paths:
                continue
            try:
                # Match the file loader: resolve file and directory symlinks.
                # A missing/inaccessible target must fail, not silently vanish.
                mode = entry.stat(follow_symlinks=True).st_mode
            except OSError as exc:
                raise RuntimeError(f"cannot stat {entry.path}: {exc}") from exc
            if stat.S_ISDIR(mode):
                ensure_dir(child_rel)
                walk(entry.path, child_rel)
                continue
            if not stat.S_ISREG(mode):
                raise RuntimeError(f"unsupported settings file: {entry.path}")
            try:
                with open(entry.path, "rb") as handle:
                    data = handle.read()
            except OSError as exc:
                raise RuntimeError(f"cannot read {entry.path}: {exc}") from exc
            parent = rel
            db.execute(
                "INSERT INTO nodes(path, parent, name, kind, ordinal, data)"
                " VALUES(?, ?, ?, 0,"
                " COALESCE((SELECT MAX(ordinal) FROM nodes WHERE parent=?), -1) + 1,"
                " ?)",
                (child_rel, parent, name, parent, data),
            )
            files += 1
        active_dirs.remove(identity)

    walk(config, "")
    return directories, files


if __name__ == "__main__":
    main()
