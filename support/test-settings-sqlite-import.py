#!/usr/bin/env python3
"""Importer checks: directory order, skipped files, and a failed read."""

import os
import sqlite3
import subprocess
import sys
import tempfile

SCRIPT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "settings-sqlite-import.py")


def run_import(config, output):
    return subprocess.run(
        [sys.executable, SCRIPT, config, "-o", output],
        capture_output=True,
        text=True,
    )


def main():
    root = tempfile.mkdtemp()
    channel = os.path.join(root, "channel")
    os.makedirs(channel)
    os.makedirs(os.path.join(root, "imagecache", "data"))
    # Create "two" first so a sorted import would disagree with scandir
    # only when the filesystem order differs. The check below is the
    # recorded order, whatever scandir returned, against a second listing.
    with open(os.path.join(channel, "two"), "wb") as handle:
        handle.write(b'{"name":"Two"}\n')
    with open(os.path.join(channel, "one"), "wb") as handle:
        handle.write(b'{"name":"One"}\n')
    with open(os.path.join(channel, ".hidden"), "w", encoding="utf-8") as handle:
        handle.write("no")
    with open(os.path.join(channel, "skip~"), "w", encoding="utf-8") as handle:
        handle.write("no")
    with open(os.path.join(root, "epgdb"), "wb") as handle:
        handle.write(b"not-settings")
    with open(os.path.join(root, "imagecache", "data", "1"), "wb") as handle:
        handle.write(b"pic")
    with open(os.path.join(root, "imagecache", "config"), "w", encoding="utf-8") as handle:
        handle.write("{}\n")

    output = os.path.join(root, "settings.sqlite.new")
    result = run_import(root, output)
    if result.returncode != 0:
        sys.exit(result.stderr or "import failed")
    db = sqlite3.connect(output)
    names = [
        row[0]
        for row in db.execute(
            "SELECT name FROM nodes WHERE parent='channel' ORDER BY ordinal"
        )
    ]
    with os.scandir(channel) as entries:
        expected = [
            entry.name
            for entry in entries
            if entry.name[:1] != "." and not entry.name.endswith("~")
        ]
    if names != expected:
        sys.exit(f"order {names} != {expected}")
    blob = db.execute("SELECT data FROM nodes WHERE path='channel/one'").fetchone()[0]
    if blob != b'{"name":"One"}\n':
        sys.exit(f"bytes changed: {blob!r}")
    for pattern in ("epgdb%", "imagecache/data%"):
        count = db.execute(
            "SELECT COUNT(*) FROM nodes WHERE path LIKE ?", (pattern,)
        ).fetchone()[0]
        if count:
            sys.exit(f"{pattern} was imported")
    if db.execute("PRAGMA integrity_check").fetchone()[0] != "ok":
        sys.exit("integrity check failed")

    blocked = os.path.join(root, "blocked")
    os.makedirs(blocked)
    target = os.path.join(blocked, "secret")
    with open(target, "w", encoding="utf-8") as handle:
        handle.write("x")
    os.chmod(target, 0)
    bad = os.path.join(root, "bad.sqlite")
    failed = run_import(blocked, bad)
    # Windows cannot make a file unreadable by clearing its mode bits.
    if os.name != "nt":
        if failed.returncode == 0:
            sys.exit("unreadable file was imported")
        if os.path.exists(bad):
            sys.exit("failed import left a database behind")
    print("importer ok")


if __name__ == "__main__":
    main()
