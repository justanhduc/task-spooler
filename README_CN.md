# Task Spooler PLUS

本项目基于 [Lluís Batlle i Rossell 的 Task Spooler](https://vicerveza.homeunix.net/~viric/soft/ts/)，增加了**多用户支持、SQLite 崩溃恢复、cgroups CPU 限制与冻结/暂停、动态用户管理**等功能。以 root 权限后台服务运行，适用于多人共享的工作站。

## 简介

在日常研究中，我经常需要在工作站上同时提交大量仿真任务，而原始的 task-spooler 不支持多用户（每个用户独立维护队列）。<u>task-spooler-PLUS</u> 通过中心化服务器实现多用户统一管理。

近期增强包括：**cgroups v1/v2 CPU 限制与 freezer 暂停/恢复**、**SQLite3 WAL 模式崩溃恢复**、基于 `vec_t` 的动态用户管理。

### 更新日志

参见 [CHANGELOG](CHANGELOG.md)。

## 特性

- **跨平台**：支持 GNU/Linux、Darwin、Cygwin、FreeBSD
- **多用户**：每个用户可配置最大槽位（CPU 核数）
- **崩溃恢复**：通过 SQLite3（WAL 模式）持久化，重启后恢复所有任务状态
- **Cgroups 支持**：CPU 配额限制 + freezer 暂停/恢复（v1/v2 编译时可选）
- **Wall-time 管理**：超时任务自动暂停并排到队尾
- **用户全局控制**：暂停/恢复单个用户的所有任务
- **多种输出格式**：默认、JSON、Tab 分隔
- **简易构建**：`make` 即可（无需 autotools）
- **stdout/stderr 分离**：方便日志管理
- **PID 查询**（`--find-by-pid`）：查找某个 PID 属于哪个任务（含子进程）

## 工具

- `tools/migrate_uid.py` — 将旧的 `ts_UID` 列（向量索引）迁移为 Linux UID
- `tools/clear_finished.py` — 清空或 `--drop` + 重建 Finished 表

## 快速开始

```bash
make                      # 编译（cgroups v1，默认）
make CGROUP_V2=1          # 编译 cgroups v2 版本
sudo ./ts --daemon        # 启动后台服务（仅 root）
./ts -l                   # 列出所有任务
./ts sleep 30             # 提交一个任务
./ts -r <id>              # 删除任务
./ts -k <id>              # 终止运行中的任务
./ts -w <id>              # 等待任务完成
```

## 编译与安装

```bash
make                      # 编译 ts 二进制文件
make CGROUP_V2=1          # 编译 cgroups v2 版本
make clean                # 清理编译产物
./install_make            # 安装到 /usr/local（需要 root）
```

**默认路径**（可通过环境变量覆盖）：

| 变量 | 默认值 | 说明 |
|------|--------|------|
| `TS_SOCKET` | `$TMPDIR/socket-ts.root` | Unix 套接字 |
| `TS_USER_PATH` | `user.txt` 所在路径 | 用户配置文件 |
| `TS_LOGFILE_PATH` | `log.txt` 所在路径 | 任务日志 |
| `TS_SQLITE_PATH` | `task-spooler.db` 所在路径 | SQLite 数据库 |
| `TS_SLOTS` | `1` | 最大并发任务数 |
| `TS_MAXFINISHED` | `1000` | 最大已完成任务数 |
| `TS_MAX_WALL_TIME` | `10080`（分钟） | 最大 wall-time 限制 |
| `TS_FIRST_JOBID` | `1000` | 起始任务 ID |

可编辑 `defaults.h` 修改内置默认值。

## 用户配置

服务器通过用户配置文件（路径由 `TS_USER_PATH` 指定）管理用户。配置格式为 **用户名 + 最大槽位数**，用户名通过 `getpwnam()` 自动解析为系统 UID，无需手动指定。

**user.txt 格式**：

```
# <用户名> <最大槽位数>
TS_SLOTS = 16
john    4
mary    2
```

root（uid=0）自动加入，拥有完全控制权。

运行时可使用 `ts -X`（仅 root）刷新配置。刷新只允许**新增**用户——已有用户不可删除或更改槽位数。

## 工作原理

**服务器进程**以 root 运行，在内存中管理任务并通过 SQLite3 持久化。**客户端进程**通过 Unix 套接字连接。服务器不执行用户命令——客户端 fork 并运行任务，保留用户环境、ulimits 和工作目录。

```
ts (客户端)  ──Unix socket──▶  ts (服务器守护进程)
   │                              │
   fork() + exec(cmd)             │  管理队列、槽位、用户
   │                              │  持久化到 SQLite3 (WAL)
   waitpid() → 通知服务器          │
```

崩溃后，运行中的任务通过 `--relink` 重新挂载。重启后，所有任务状态从 SQLite 恢复。

### Cgroups 支持

编译时选择：
- `make` — cgroups v1（`cpu.cfs_quota_us` + `freezer.state`）
- `make CGROUP_V2=1` — cgroups v2（`cpu.max` + `cgroup.freeze`）

两者均提供 CPU 配额限制和 freezer 暂停/恢复功能。

### 单实例保护

`--daemon` 启动时扫描 `/proc`：如发现同路径的二进制文件已被 root 运行，拒绝启动。

### 常见问题

- **服务器卡住**：删除套接字文件（`/tmp/socket-ts.root`）后重启
- **SIGKILL 后残留**：`.db-wal` 和 `.db-shm` 文件保留——SQLite 下次打开时自动恢复
- **崩溃后**：运行中的任务丢失退出码和信号信息

## 命令参考

运行 `ts -h` 查看完整帮助。

```
Task Spooler 2.1.1a - Unix 用户任务队列系统
Copyright (C) 2007-2024  Kylin JIANG - Duc Nguyen - Lluis Batlle i Rossell

环境变量：
  TS_SOCKET        : Unix 套接字路径（默认：$TMPDIR/socket-ts.root）
  TS_SLOTS         : 最大并发任务数（服务器启动，默认：1）
  TS_USER_PATH     : 用户配置文件路径（服务器启动）
  TS_LOGFILE_PATH  : 任务日志路径（服务器启动）
  TS_SQLITE_PATH   : SQLite 数据库路径（服务器启动）
  TS_MAXFINISHED   : 最大已完成任务数（默认：1000）
  TS_MAX_WALL_TIME : 最大 wall-time（默认：10080 分钟）
  TS_MAXCONN       : 最大连接数（默认：1000）
  TS_SORTJOBS      : 任务队列排序控制
  TS_SAVELIST      : 崩溃恢复任务列表文件
  TS_ONFINISH      : 任务完成后执行的二进制文件
  TS_ENV           : 提交任务时收集环境信息的命令
  TS_MAIL_FROM     : 结果邮件发件人
  TS_MAIL_TIME     : 邮件通知阈值（秒）
  TMPDIR           : 临时输出目录

长选项操作：
  --getenv [var]          获取服务器环境变量
  --setenv [var]          设置服务器环境变量
  --unsetenv [var]        删除服务器环境变量
  --get_label || -a [id]  显示任务标签
  --full_cmd || -F [id]   显示完整命令
  --find-by-pid [pid]     查找 PID 属于哪个运行中的任务（含子进程）
  --check_daemon           验证守护进程状态
  --count_running || -R   统计运行中的任务数
  --last_queue_id || -q   显示最后添加的任务 ID
  --get_logdir             显示日志目录路径
  --set_logdir [path]     配置日志目录
  --serialize || -M [fmt] 导出任务列表（default/json/tab）
  --hold [jobid]          暂停指定任务
  --cont [jobid]          恢复暂停的任务
  --suspend [USER]        暂停用户
  --resume [USER]         恢复用户
  --lock                  锁定服务器
  --unlock                解除服务器锁定
  --relink [pid]          崩溃后重新挂载任务
  --wtime [dur]           设置 wall-time 限制（如 30s, 3.4m, 1.5H, 2d）
  --add_wtime [dur]       增加任务 wall-time（仅 root）
  --job [id] || -J [id]  指定任务 ID
  --daemon                以守护进程模式运行（仅 root）

操作：
  -A           显示所有用户信息
  -X           刷新用户配置（仅 root）
  -K           停止服务器（仅 root）
  -C           清空已完成任务
  -l           显示任务列表（默认）
  -S [num]     获取/设置最大并发任务数（仅 root）
  -t [id]      查看最后 10 行输出
  -c [id]      查看完整输出
  -p [id]      显示任务 PID
  -o [id]      显示输出文件路径
  -i [id]      显示任务详细信息
  -s [id]      显示任务状态
  -r [id]      删除任务
  -w [id]      等待任务完成
  -k [id]      向任务发送 SIGTERM
  -T           向所有任务发送 SIGTERM（仅 root）
  -u [id]      提升任务优先级
  -U <id-id>   交换两个任务的位置
  -h           显示帮助
  -V           显示版本

添加任务的选项：
  -B           服务器满时退出
  -n           不存储输出
  -E           分离 stderr
  -O           设置日志文件名
  -z           Gzip 压缩输出
  -f           前台运行
  -m <email>   邮件通知
  -d           在上一个任务后运行
  -D <id,...>  在指定 ID 后运行
  -W <id,...>  在指定 ID 成功后运行
  -L [label]   设置任务标签
  -N [num]     所需槽位数（默认：1）
```

## 致谢

- Андрей Пантюхин (Andrew Pantyukhin) 维护 BSD 移植版
- Alessandro Öhler 提供原始 Gentoo ebuild
- Alexander V. Inyukhin 维护非官方 Debian 包
- Pascal Bleser 为 SuSE/openSuSE 打包
- Gnomeye 维护 AUR 包
- Eric Keller 编写了 task spooler 队列的 nodejs web 服务器
- Duc Nguyen 开发了 GPU 支持
- **Kylin JIANG** 添加了：多用户支持、SQLite3 崩溃恢复、cgroups CPU/freezer（v1+v2）、动态用户管理、PID 查询及多项稳定性修复

## 许可证

参见项目中的 `COPYING` 文件。
