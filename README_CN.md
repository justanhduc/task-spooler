# Task Spooler PLUS

基于 [Lluís Batlle i Rossell 的 Task Spooler](https://vicerveza.homeunix.net/~viric/soft/ts/) 增强开发，新增**多用户支持、SQLite3 崩溃恢复、cgroups v1/v2 资源限制**。为个人工作站和小型共享服务器设计——SLURM/PBS 的轻量替代。

## 特性

- **多用户** — 每个用户独立 slot 配额
- **崩溃恢复** — SQLite3 持久化，重启后恢复所有任务（运行中/排队/已完成）
- **Cgroups CPU 限制 + 冻结/解冻** — 编译时可选 v1 或 v2
- **Wall-time 管理** — 超时任务自动暂停，排到队尾延迟执行
- **PID 查找**（`--find-by-pid`）— 查找进程属于哪个任务（含子进程/孙进程）
- **多种输出格式** — 默认、JSON、Tab
- **用户刷新验证** — refresh 只能增加用户，不能删除或修改
- **单实例保护** — /proc 扫描防止同时运行两个服务

## 快速开始

```bash
make                      # 编译（cgroups v1，默认）
make CGROUP_V2=1          # 编译 cgroups v2 版本
./install_make            # 安装到 /usr/local（需要 root）
```

服务以 **root** 运行。客户端通过 Unix socket 连接（默认 `/tmp/socket-ts.root`）。

## 用户配置

格式：`<用户名> <最大槽位数>`，用户名必须在系统中存在（通过 `getpwnam` 解析 UID）。

```
# user.txt（通过 TS_USER_PATH 环境变量指定路径）
TS_SLOTS = 16
john    4
mary    2
```

## 环境变量

| 变量 | 用途 | 默认值 |
|------|------|--------|
| `TS_SOCKET` | Unix socket 路径 | `$TMPDIR/socket-ts.root` |
| `TS_SLOTS` | 最大并发任务数 | `1` |
| `TS_USER_PATH` | 用户配置文件路径 | `user.txt` |
| `TS_SQLITE_PATH` | SQLite 数据库路径 | `task-spooler.db` |
| `TS_LOGFILE_PATH` | 任务日志路径 | `log.txt` |
| `TS_MAXFINISHED` | 内存中最多已完成任务数 | `1000` |
| `TS_MAX_WALL_TIME` | 最大 wall-time（秒） | `604800`（7 天） |
| `TS_MAXCONN` | 最大连接数 | `1000` |
| `TS_SORTJOBS` | 排序控制 | `0` |
| `TS_SAVELIST` | 崩溃恢复转储文件 | 无 |
| `TS_FIRST_JOBID` | 起始任务 ID | `1000` |
| `TMPDIR` | 临时输出目录 | `/tmp` |

## 编译选项

```bash
make                      # cgroups v1（默认）
make CGROUP_V2=1          # cgroups v2 — 统一 /sys/fs/cgroup/
```

## 架构

**客户端-服务器，Unix socket 通信。** 服务器管理队列；客户端 fork 并执行任务。

```
ts (客户端) ──Unix socket──▶  ts (服务端)
   │                              │
   fork() + exec(cmd)             │  队列、slots、用户管理
   │                              │  SQLite3 持久化
   waitpid() ➔ 通知服务端          │  cgroups CPU + freezer
```

### 核心文件

| 文件 | 用途 |
|------|------|
| `main.c` | 命令行解析、分发 |
| `server.c` | `select()` 事件循环 |
| `jobs.c` | 任务队列、状态机、slot 统计 |
| `execute.c` | `fork`+`exec`，捕获退出码 |
| `cgroups.c` | CPU 限制 + 冻结/解冻（v1/v2） |
| `sqlite.c` | SQLite3 WAL 模式持久化 |
| `user.c` | `vec_t users_vec` + `struct User` 动态管理 |

## 使用说明

```
操作:
  -l           列出任务（默认）
  -N [num]     所需槽位数
  -L [label]   任务标签
  -d           依赖上一个任务
  -D <id,...>  依赖指定任务
  -w [id]      等待任务完成
  -r [id]      删除任务
  -k [id]      发送 SIGTERM
  -t [id]      查看输出末尾
  -c [id]      查看全部输出
  -p [id]      显示任务 PID
  -P <pid>     查找 PID 属于哪个任务（含子进程）
  -i [id]      任务详情
  -s [id]      任务状态
  -u [id]      紧急（移到队首）
  -U <id-id>   交换两个任务
  -C           清除已完成任务
  -S [num]     获取/设置最大槽位（root）
  -K           停止服务（root）
  -X           刷新用户配置（root）
  -A           列出所有用户
  -R           统计运行中任务
  -q           最后任务 ID
  -V           版本
  -h           帮助
  --daemon     以守护进程启动（root）
  --find-by-pid <pid>  按进程 PID 查找任务
  --hold [id]  暂停任务
  --cont [id]  恢复任务
  --suspend [user]  暂停用户
  --resume [user]   恢复用户
  --wtime <t>  Wall-time 限制（如 30s, 3.4m, 1.5H, 2d）
  --add_wtime <t>  增加 wall-time（root）
  --getenv <var>   获取环境变量
  --setenv <var>   设置环境标记
  --unsetenv <var> 删除环境标记
  --tmp        输出存到 /tmp
  --serialize <fmt>  导出列表（json/tab）
```

## 历史

- **Kylin JIANG** — 多用户支持、SQLite 崩溃恢复、cgroups v1/v2
- **Duc Nguyen** — GPU 支持
- **Lluís Batlle i Rossell** — 原始 task-spooler
