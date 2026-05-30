#!/usr/bin/env python3
"""Clear or selectively delete rows from the Finished table.

Usage:
    python3 tools/clear_finished.py task-spooler.db          # delete all
    python3 tools/clear_finished.py task-spooler.db 1002     # delete jobid 1002
    python3 tools/clear_finished.py task-spooler.db 1000-1005  # delete range
"""

import sys
import sqlite3


def parse_range(s):
    """Parse 'N' or 'N-M' into (start, end) inclusive."""
    if '-' in s:
        a, b = s.split('-', 1)
        return int(a), int(b)
    return int(s), int(s)


def clear_finished(db_path, lo=None, hi=None):
    conn = sqlite3.connect(db_path)

    cur = conn.execute(
        "SELECT name FROM sqlite_master WHERE type='table' AND name='Finished'"
    )
    if not cur.fetchone():
        print("No 'Finished' table found.")
        conn.close()
        return

    if lo is not None:
        conn.execute(
            "DELETE FROM Finished WHERE jobid BETWEEN ? AND ?", (lo, hi)
        )
        print(f"Deleted Finished rows jobid {lo} to {hi}")
    else:
        cur = conn.execute("SELECT COUNT(*) FROM Finished")
        n = cur.fetchone()[0]
        conn.execute("DELETE FROM Finished")
        print(f"Deleted all {n} rows from Finished")
        conn.execute("VACUUM")

    conn.commit()
    conn.close()


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(f"Usage:  python3 {sys.argv[0]} task-spooler.db [jobid|-range]")
        print(f"        python3 {sys.argv[0]} task-spooler.db           # delete all")
        print(f"        python3 {sys.argv[0]} task-spooler.db 1002      # delete one")
        print(f"        python3 {sys.argv[0]} task-spooler.db 1000-1005 # delete range")
        sys.exit(1)

    db_path = sys.argv[1]
    lo = hi = None
    if len(sys.argv) >= 3:
        lo, hi = parse_range(sys.argv[2])

    clear_finished(db_path, lo, hi)
