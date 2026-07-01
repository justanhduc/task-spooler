#!/usr/bin/env python3
"""Clear or selectively delete rows from the Finished table.

Usage:
    python3 tools/clear_finished.py task-spooler.db              # delete all
    python3 tools/clear_finished.py task-spooler.db 1002         # delete jobid 1002
    python3 tools/clear_finished.py task-spooler.db 1000-1005    # delete range
    python3 tools/clear_finished.py task-spooler.db --drop       # DROP + recreate
"""

import sys
import sqlite3


FINISHED_SCHEMA = """\
CREATE TABLE IF NOT EXISTS Finished(
jobid INT PRIMARY KEY     NOT NULL,
command           TEXT    NOT NULL,
state             INT     NOT NULL,
output_filename   TEXT    NOT NULL,
store_output      INT     NOT NULL,
pid               INT     NOT NULL,
ts_UID            INT     NOT NULL,
should_keep_finished INT NOT NULL,
depend_on         INT     NOT NULL,
depend_on_size    INT     NOT NULL,
notify_errorlevel_to INT   NOT NULL,
notify_errorlevel_to_size INT NOT NULL,
dependency_errorlevel INT NOT NULL,
label TEXT NOT NULL,
email TEXT NOT NULL,
num_slots INT NOT NULL,
errorlevel INT NOT NULL, died_by_signal INT NOT NULL, signal INT NOT NULL,
user_sec INT NOT NULL, system_sec INT NOT NULL, real_sec INT NOT NULL, skipped INT NOT NULL,
ptr TEXT NOT NULL, nchars INT NOT NULL, allocchars INT NOT NULL, wall_time INT NOT NULL,
enqueue_time INT NOT NULL, start_time INT NOT NULL, end_time INT NOT NULL,
pause_time INT NOT NULL, pause_duration INT NOT NULL, end_time_ms INT NOT NULL,
order_id INT NOT NULL, command_strip INT NOT NULL, work_dir TEXT NOT NULL)"""


def parse_range(s):
    if '-' in s:
        a, b = s.split('-', 1)
        return int(a), int(b)
    return int(s), int(s)


def drop_finished(conn):
    cur = conn.execute("SELECT name FROM sqlite_master WHERE type='table' AND name='Finished'")
    if not cur.fetchone():
        print("No 'Finished' table found.")
        return
    conn.execute("DROP TABLE Finished")
    conn.execute(FINISHED_SCHEMA)
    conn.commit()
    print("Dropped and recreated Finished table.")


def clear_finished(db_path, lo=None, hi=None, drop=False):
    conn = sqlite3.connect(db_path)

    if drop:
        drop_finished(conn)
        conn.close()
        return

    cur = conn.execute("SELECT name FROM sqlite_master WHERE type='table' AND name='Finished'")
    if not cur.fetchone():
        print("No 'Finished' table found.")
        conn.close()
        return

    if lo is not None:
        conn.execute("DELETE FROM Finished WHERE jobid BETWEEN ? AND ?", (lo, hi))
        print(f"Deleted Finished rows jobid {lo} to {hi}")
    else:
        cur = conn.execute("SELECT COUNT(*) FROM Finished")
        n = cur.fetchone()[0]
        conn.execute("DELETE FROM Finished")
        conn.execute("VACUUM")
        print(f"Deleted all {n} rows from Finished")

    conn.commit()
    conn.close()


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(f"Usage:  python3 {sys.argv[0]} task-spooler.db [jobid|-range|--drop]")
        print(f"        python3 {sys.argv[0]} task-spooler.db           # delete all")
        print(f"        python3 {sys.argv[0]} task-spooler.db 1002      # delete one")
        print(f"        python3 {sys.argv[0]} task-spooler.db 1000-1005 # delete range")
        print(f"        python3 {sys.argv[0]} task-spooler.db --drop    # DROP + recreate")
        sys.exit(1)

    db_path = sys.argv[1]
    lo = hi = None
    drop = False
    if len(sys.argv) >= 3:
        if sys.argv[2] == "--drop":
            drop = True
        else:
            lo, hi = parse_range(sys.argv[2])

    clear_finished(db_path, lo, hi, drop)
