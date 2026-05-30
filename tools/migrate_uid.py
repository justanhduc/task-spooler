#!/usr/bin/env python3
"""Migrate ts_UID column in task-spooler SQLite database.

The old format stored a user-list index (0=root, 1=first user, ...).
The new format stores the Linux UID directly.

Fill in the MAPPING dict below, then run:
    python3 tools/migrate_uid.py task-spooler.db
"""

import sys
import sqlite3

# ─── Edit this mapping ──────────────────────────────────────
# old ts_UID index → Linux UID
# 0 is always root (uid 0), add your users below.
MAPPING = {
    0: 0,       # root
    1: 1000,    # first user
    2: 1001,    # second user
    # ...
}
# ────────────────────────────────────────────────────────────


def migrate(db_path, mapping):
    conn = sqlite3.connect(db_path)

    for table in ("Jobs", "Finished"):
        cur = conn.execute(
            "SELECT name FROM sqlite_master WHERE type='table' AND name=?",
            (table,),
        )
        if not cur.fetchone():
            print(f"  Table '{table}' not found, skipping")
            continue

        rows = conn.execute(
            f"SELECT jobid, ts_UID FROM {table}"
        ).fetchall()

        count = 0
        for jobid, old_val in rows:
            if old_val in mapping:
                new_val = mapping[old_val]
                conn.execute(
                    f"UPDATE {table} SET ts_UID=? WHERE jobid=?",
                    (new_val, jobid),
                )
                count += 1
                print(f"  [{table}] job {jobid}: {old_val} → {new_val}")
            else:
                print(f"  [{table}] job {jobid}: ts_UID={old_val} unmapped, skipped")

        print(f"  {table}: {count} row(s) updated\n")

    conn.commit()
    conn.close()
    print("Done.")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(f"Usage:  python3 {sys.argv[0]} task-spooler.db")
        sys.exit(1)
    print(f"Database: {sys.argv[1]}")
    print(f"Mapping:  {MAPPING}\n")
    migrate(sys.argv[1], MAPPING)
