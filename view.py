import sqlite3

conn = sqlite3.connect("task-spooler.db")
cursor = conn.cursor()

# 获取表的列信息
cursor.execute("PRAGMA table_info(JObs)")
columns = cursor.fetchall()

# 打印列名
#for col in columns:
#    print(col[1])  # col[1] 是列名

# 或者提取所有列名到列表
column_names = [col[1] for col in columns]
print(column_names)
conn.close()

conn = sqlite3.connect("task-spooler.db")
cursor = conn.cursor()

cursor.execute("SELECT * FROM Jobs")

rows = cursor.fetchall()
for row in rows:
    print(row)

conn.close()