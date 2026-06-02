#!/usr/bin/env python3
"""
upgrade_db.py — ts SQLite 数据库升级脚本

备份原数据库，然后添加旧版本缺失的字段。

用法:
  python3 upgrade_db.py <数据库路径>

示例:
  python3 upgrade_db.py /tmp/ts.sqlite
  python3 upgrade_db.py ./task-spooler.db
"""

import sys
import os
import shutil
import sqlite3

# 需要添加的字段：(表名, 字段名, 类型定义)
# 按添加顺序排列，每个字段只在表中不存在时才添加
MISSING_COLUMNS = [
    ("Jobs",     "schedule_time", "INT NOT NULL DEFAULT 0"),
    ("Finished", "schedule_time", "INT NOT NULL DEFAULT 0"),
]


def upgrade(db_path):
    if not os.path.exists(db_path):
        print(f"Error: database not found: {db_path}")
        sys.exit(1)

    # 备份
    bak_path = db_path + ".bak"
    if not os.path.exists(bak_path):
        shutil.copy2(db_path, bak_path)
        print(f"Backup: {bak_path}")
    else:
        print(f"Backup exists (skipped): {bak_path}")

    conn = sqlite3.connect(db_path)
    c = conn.cursor()
    changed = False

    for table, col_name, col_def in MISSING_COLUMNS:
        try:
            c.execute("PRAGMA table_info(%s)" % table)
            existing = [row[1] for row in c.fetchall()]
        except Exception as e:
            print(f"  [{table}] error reading schema: {e}")
            continue

        if col_name in existing:
            print(f"  [{table}] {col_name} — already exists")
        else:
            sql = "ALTER TABLE %s ADD COLUMN %s %s" % (table, col_name, col_def)
            try:
                c.execute(sql)
                print(f"  [{table}] {col_name} — ADDED")
                changed = True
            except Exception as e:
                print(f"  [{table}] {col_name} — FAILED: {e}")

    if changed:
        conn.commit()
    conn.close()
    print("Done.")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)
    upgrade(sys.argv[1])
