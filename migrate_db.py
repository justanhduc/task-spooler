#!/usr/bin/env python3
"""
Migrate old task-spooler SQLite DB to new format.

Differences (both Jobs and Finished tables):
  - user_ms/system_ms/real_ms (FLOAT, ms) → user_sec/system_sec/real_sec (INT, seconds)
  - enqueue_time_ms, start_time_ms dropped
  - wall_time, pause_time, pause_duration added (default 0)

Usage:
  python3 migrate_db.py old.db new.db

The new.db is updated in-place (WAL mode, write-ahead).  Backup first.
migration from 2.1.2 to 2.1.3 versions with wall-time limits
"""

import sqlite3
import sys
import time

def migrate_table(src_conn, dst_conn, table_name):
    """Copy rows from old table to new table with schema conversion."""
    src = src_conn.execute(f"SELECT * FROM {table_name} ORDER BY jobid")
    src_cols = [c[1] for c in src_conn.execute(f"PRAGMA table_info({table_name})").fetchall()]
    old_rows = src.fetchall()

    if not old_rows:
        print(f"  {table_name}: empty, skipping")
        return

    unix_sec_int = int(time.time())
    monotonic_sec = int(time.monotonic())
    start_time = unix_sec_int - monotonic_sec
    # Build new rows with correct column mapping
    new_rows = []
    for row in old_rows:
        d = dict(zip(src_cols, row))
        new_rows.append((
            d["jobid"],                              # 0
            d["command"],                            # 1
            d["state"],                              # 2
            d["output_filename"],                    # 3
            d["store_output"],                       # 4
            d["pid"],                                # 5
            d["ts_UID"],                             # 6
            d["should_keep_finished"],               # 7
            d["depend_on"],                          # 8
            d["depend_on_size"],                     # 9
            d["notify_errorlevel_to"],               # 10
            d["notify_errorlevel_to_size"],          # 11
            d["dependency_errorlevel"],              # 12
            d["label"],                              # 13
            d["email"],                              # 14
            d["num_slots"],                          # 15
            d["errorlevel"],                         # 16
            d["died_by_signal"],                     # 17
            d["signal"],                             # 18
            int(d["user_ms"]),                       # 19: user_ms(ms) → user_sec(s)
            int(d["system_ms"]),                     # 20: system_ms(ms) → system_sec(s)
            int(d["real_ms"]),                       # 21: real_ms(ms) → real_sec(s)
            d["skipped"],                            # 22
            d["ptr"],                                # 23
            d["nchars"],                             # 24
            d["allocchars"],                         # 25
            604800,                                       # 26: wall_time (new, default 0)
            int(d["enqueue_time"])-start_time,                       # 27 from unix-time to monotonic time
            int(d["start_time"])-start_time,                         # 28
            int(d["end_time"])-start_time,                           # 29
            0,                                       # 30: pause_time (new, default 0)
            0,                                       # 31: pause_duration (new, default 0)
            d["end_time_ms"],                        # 32
            d["order_id"],                           # 33
            d["command_strip"],                      # 34
            d["work_dir"],                           # 35
        ))

    placeholders = ", ".join(["?"] * 36)
    dst_conn.executemany(
        f"INSERT OR REPLACE INTO {table_name} VALUES ({placeholders})",
        new_rows,
    )
    print(f"  {table_name}: migrated {len(new_rows)} rows")


def migrate_global(src_conn, dst_conn):
    """Merge Global JOBIDs: take the max of both."""
    old_row = src_conn.execute("SELECT * FROM Global WHERE id = 1").fetchone()
    new_row = dst_conn.execute("SELECT * FROM Global WHERE id = 1").fetchone()

    old_jobid = old_row[1] if old_row else 0
    new_jobid = new_row[1] if new_row else 0
    final_jobid = max(old_jobid, new_jobid)

    dst_conn.execute("INSERT OR REPLACE INTO Global VALUES (1, ?)", (final_jobid,))
    print(f"  Global: JOBIDs set to {final_jobid} (old={old_jobid}, new={new_jobid})")


def main():
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} old.db new.db", file=sys.stderr)
        sys.exit(1)

    old_path, new_path = sys.argv[1], sys.argv[2]

    src = sqlite3.connect(old_path)
    dst = sqlite3.connect(new_path)

    print("Migrating Jobs...")
    migrate_table(src, dst, "Jobs")

    print("Migrating Finished...")
    migrate_table(src, dst, "Finished")

    print("Migrating Global...")
    migrate_global(src, dst)

    dst.commit()
    src.close()
    dst.close()
    print("Done.")


if __name__ == "__main__":
    main()
