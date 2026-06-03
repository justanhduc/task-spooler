# Task Spooler PLUS

**一个二进制文件搞定多用户任务调度**——你可以把它当成共享工作站的轻量级 Slurm。没有守护进程集群、不用配数据库、不需要任何集群基础设施，一个 `ts` 就够了。

最初只是一个单用户的任务排队工具，**Kylin JIANG** 把它重构成了一个具备崩溃恢复、cgroups 资源隔离、NUMA 感知 CPU 绑定的多用户调度器，让小型共享机器也能享受 Slurm 级别的调度能力。

[English](README.md)

## 为什么选 TS PLUS？

### 相比原版 Task Spooler

| | 原版 | TS PLUS |
|---|---|---|
| **用户模型** | 各管各的，每人一个独立队列 | 一个中心服务端统一调度，支持按用户限制槽位 |
| **崩溃恢复** | 任务丢了就没了 | SQLite3 WAL 持久化，任务、状态、耗时数据跨崩溃和重启全保留 |
| **资源管控** | 没有 | cgroups v1/v2：CPU 配额、freezer 暂停/恢复、NUMA CPU 绑定 |
| **调度策略** | 只能先进先出 | 依赖链、超时自动暂停、`--at` 定时执行 |
| **用户管理** | 没有 | 配置文件热加载（`ts -X`），支持按用户暂停/恢复 |
| **容错能力** | 服务端一挂客户端全断 | 客户端自动重连，运行中的任务无缝恢复 |

### 相比 Slurm

| | Slurm | TS PLUS |
|---|---|---|
| **部署** | `slurmctld` + `slurmd` + `munge` + MySQL + 一堆配置文件 | 一个二进制，一个用户配置文件 |
| **定位** | 成百上千节点的集群 | 一两台工作站，几个人到几十个人用 |
| **多用户** | ✓ | ✓ |
| **任务恢复** | 靠外部数据库 | SQLite3 WAL，自带 |
| **CPU/NUMA 绑定** | `--cpu-bind` / `--mem-bind` | `TS_CPU_BIND=1` 编译即开，NUMA 感知分配 |
| **cgroups** | v1/v2（靠插件） | v1/v2（编译进二进制） |
| **时间限制** | ✓ | ✓（超时自动暂停，加时后重新排队） |
| **任务回调** | Epilog/Prolog 脚本 | `--on-finish` 搭配占位符，直接在用户态执行 |

如果你有一台共享工作站，几到几十个用户在上面跑仿真、炼丹、做实验，TS PLUS 能给你 Slurm 级别的调度体验，但运维成本几乎为零。

## 起源

Task Spooler PLUS 从 [Lluís Batlle i Rossell 的 Task Spooler](https://vicerveza.homeunix.net/~viric/soft/ts/) fork 而来。**Kylin JIANG** 主导了从单用户排队脚本到多用户调度器的全部改造，核心工作包括：

- **cgroups v1/v2 全支持**：CPU 配额限制、freezer 暂停/恢复、cpuset NUMA 绑定，编译时一键切换
- **SQLite3 崩溃恢复**：任务、状态、耗时全部持久化，重启不掉任何数据
- **NUMA 感知的 CPU 绑定分配器**：拓扑自动探测、best-fit 选组、自动碎片整理且不破坏 NUMA 亲和性
- **动态用户管理**：`vec_t` 承载 `struct User`，配置文件热加载，支持单用户一键暂停/恢复
- **超时自动处理**：超时任务自动暂停，加时后重新入队，不丢不挂
- **客户端自动重连**：服务端重启后客户端自动接上，运行中的任务不受影响

### 更新日志

详见 [CHANGELOG](CHANGELOG.md)。

## 功能一览

- **跨平台**：GNU/Linux、Darwin、Cygwin、FreeBSD 都能跑
- **多用户**：每个用户独立配置最大槽位（CPU 核数）
- **崩溃恢复**：SQLite3 WAL 模式，重启后所有任务状态完整恢复
- **cgroups 集成**：CPU 配额限制 + freezer 暂停/恢复（v1/v2，编译时选）
- **超时管理**：任务超时自动冻结并排到队尾
- **用户级控制**：一键暂停/恢复某个用户的所有任务
- **多格式输出**：默认、JSON、Tab 分隔三种格式
- **构建简单**：`make` 一把梭，不用 autotools
- **stdout/stderr 分离**：日志管理更清爽
- **PID 反查**（`--find-by-pid`）：给定一个 PID，查出它属于哪个任务（含子进程）
- **定时执行**（`--at`）：支持 `+5m`、`14:00`、`2025-06-01T14:00` 等格式
- **任务回调**（`--on-finish`）：任务完成后自动执行命令，通过占位符拿任务信息
- **CPU 绑定**（`TS_CPU_BIND`）：NUMA 感知的拓扑分配 + cgroups cpuset，支持 HT 排除

## 工具

- `tools/migrate_uid.py`：旧版 `ts_UID` 迁移为 Linux UID
- `tools/clear_finished.py`：清空 / 重建 Finished 表
- `tools/gen_topology.py`：一键生成 CPU 拓扑头文件，自动跳过平凡/重复策略

## 快速开始

```bash
make                              # 编译（默认 cgroups v1）
make CGROUP_V2=1                  # 换成 cgroups v2
make TS_CPU_BIND=1                # 加上 NUMA CPU 绑定
make CGROUP_V2=1 TS_CPU_BIND=1   # 全都上
sudo ./ts --daemon                # 启动服务端（需要 root）
./ts -l                           # 看看队列
./ts sleep 30                     # 扔个任务进去
./ts -r <id>                      # 删掉一个任务
./ts -k <id>                      # 干掉一个在跑的任务
./ts -w <id>                      # 等任务跑完
```

### 回调示例

`--on-finish` 可以在任务结束后自动执行命令，通过占位符拿到任务的全部信息：

```bash
./ts --on-finish "cat > job-{jobid}.info << 'EOF'
 pid={pid}  label={label}  exitcode={exitcode}
 realtime={realtime} usertime={usertime} systime={systime} pausetime={pausetime}
 start_time={start_time}
 enque_time={enque_time}
 end_time={end_time}
 slots={slots}
EOF" -L test_job sleep 10
```

任务跑完后，`job-<id>.info` 里就是解析后的完整信息——用来记日志、发邮件（`--on-finish "sendmail {exitcode} ..."`）、串联工作流都很方便。

## 编译与安装

```bash
make                      # 编译
make CGROUP_V2=1          # cgroups v2 编译
make TS_CPU_BIND=1        # CPU 绑定编译
make clean                # 清理
./install_make            # 装到 /usr/local（需要 root）
```

**默认路径**（都能用环境变量覆盖）：

| 变量 | 默认值 | 说明 |
|------|--------|------|
| `TS_SOCKET` | `$TMPDIR/socket-ts.root` | Unix 套接字 |
| `TS_USER_PATH` | `user.txt` 所在路径 | 用户配置 |
| `TS_LOGFILE_PATH` | `log.txt` 所在路径 | 任务日志 |
| `TS_SQLITE_PATH` | `task-spooler.db` 所在路径 | SQLite 数据库 |
| `TS_SLOTS` | `1` | 最大并发数 |
| `TS_MAXFINISHED` | `1000` | 最多保留多少已完成任务 |
| `TS_MAX_WALL_TIME` | `10080`（分钟） | 单任务最大运行时长 |
| `TS_FIRST_JOBID` | `1000` | 起始任务 ID |

也可以直接改 `defaults.h` 里的默认值。

## 用户配置

服务端启动时读取用户配置文件（路径由 `TS_USER_PATH` 指定）。格式很简单：**用户名 + 最大槽位数**。用户名通过 `getpwnam()` 自动解析为系统 UID，不用自己查。

**user.txt 示例**：

```
# <用户名> <最大槽位数>
TS_SLOTS = 16
john    4
mary    2
```

root（uid=0）自动加入，拥有全部权限。

运行时用 `ts -X`（需要 root）可以热加载配置。注意：热加载只允许**新增**用户，已有的不能删也不能改槽位。

## 工作原理

**服务端**以 root 身份跑，在内存里管理所有任务，同时通过 SQLite3 持久化到磁盘。**客户端**通过 Unix 套接字连上来。服务端自己不跑任何用户命令——客户端 fork 出来执行，保留用户原本的环境变量、ulimits 和工作目录。

```
ts (客户端)  ──Unix socket──▶  ts (服务端)
   │                              │
   fork() + exec(cmd)             │  管队列、管槽位、管用户
   │                              │  SQLite3 WAL 持久化
   waitpid() → 通知服务端           │
```

服务端崩了？客户端会自动重连，把跑着的任务重新挂上去。机器重启了？SQLite 里什么都在，起来继续。

### cgroups 支持

编译的时候选：
- `make` → cgroups v1（`cpu.cfs_quota_us` + `freezer.state`）
- `make CGROUP_V2=1` → cgroups v2（`cpu.max` + `cgroup.freeze`）

不管哪个版本，CPU 配额限制和 freezer 暂停/恢复都有。

要知道自己的系统是 v1 还是 v2：
```bash
mount | grep cgroup
# v1 会显示: cgroup on /sys/fs/cgroup/cpu, freezer, cpuset ...
# v2 会显示: cgroup2 on /sys/fs/cgroup type cgroup2
```

### CPU 绑定（TS_CPU_BIND）

编译时加上 `TS_CPU_BIND=1`，自动探测 NUMA 拓扑，把任务绑到最优的 CPU 组上。默认排除超线程，想带上就 `--ht`。每次任务结束后自动碎片整理，尽量保持 NUMA 亲和性。

### 单实例保护

`--daemon` 启动时会扫 `/proc`，发现同路径已经有 root 在跑了就直接拒绝，防止开了多个服务端把队列搞乱。

### 常见问题

- **服务端卡死了**：删掉 socket 文件（`/tmp/socket-ts.root`），重开就行
- **被 SIGKILL 了**：`.db-wal` 和 `.db-shm` 文件还在，SQLite 下次打开自动恢复
- **崩溃之后**：正在跑的任务会丢失退出码和信号信息（进程没了），但任务本身能从 SQLite 恢复

## 命令参考

跑 `ts -h` 看完整帮助。

```
Task Spooler PLUS 2.6.1 - 多用户任务调度器，类似 Slurm
Copyright (C) 2007-2026  Kylin JIANG - Duc Nguyen - Lluis Batlle i Rossell

环境变量：
  TS_SOCKET        : Unix 套接字路径（默认：$TMPDIR/socket-ts.root）
  TS_SLOTS         : 最大并发任务数（服务端启动，默认：1）
  TS_USER_PATH     : 用户配置文件路径（服务端启动）
  TS_LOGFILE_PATH  : 任务日志路径（服务端启动）
  TS_SQLITE_PATH   : SQLite 数据库路径（服务端启动）
  TS_MAXFINISHED   : 最大已完成任务数（默认：1000）
  TS_MAX_WALL_TIME : 最大 wall-time（默认：10080 分钟）
  TS_MAXCONN       : 最大连接数（默认：1000）
  TS_SORTJOBS      : 任务队列排序控制
  TS_SAVELIST      : 崩溃恢复任务列表文件
  TS_ENV           : 提交任务时收集环境信息的命令
  TS_ONFINISH      : 默认的任务完成回调（可被 --on-finish 覆盖）
  TMPDIR           : 临时输出目录

长选项操作：
  --getenv [var]          获取服务端环境变量
  --setenv [var]          设置服务端环境变量
  --unsetenv [var]        删除服务端环境变量
  --get-label || -a [id]  显示任务标签
  --full-cmd || -F [id]   显示完整命令
  --find-by-pid [pid]     查找 PID 属于哪个运行中的任务（含子进程）
  --check-daemon           验证守护进程状态
  --count-running || -R   统计运行中的任务数
  --last-queue-id || -q   显示最后添加的任务 ID
  --get-logdir             显示日志目录路径
  --set-logdir [path]     配置日志目录
  --serialize || -M [fmt] 导出任务列表（default/json/tab）
                          用 -M json -J <id> 导出单个任务 JSON
  --hold [jobid]          暂停指定任务
  --cont [jobid]          恢复暂停的任务
  --suspend [USER]        暂停用户
  --resume [USER]         恢复用户
  --lock                  锁定服务端
  --unlock                解除服务端锁定
  --at <时间>              排期运行：+5m, 14:00, 06-01_14:00, 2025-06-01T14:00
  --on-finish <模板>      任务结束后运行命令
                          占位符：{jobid} {output} {exitcode} {pid} {label}
                          {command} {realtime} {usertime} {systime}
                          {pausetime} {start_time} {enque_time} {end_time} {slots}
  --wtime [dur]           设置 wall-time 限制（如 30s, 3.4m, 1.5H, 2d）
  --add-wtime [dur]       增加任务 wall-time（仅 root）
  --job [id] || -J [id]  指定任务 ID
  --daemon                以守护进程模式运行（仅 root）

操作：
  -A           显示所有用户信息
  -X           刷新用户配置（仅 root）
  -K           停止服务端（仅 root）
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
  -B           服务端满时直接退出
  -n           不存输出
  -E           分离 stderr
  -O           自定义日志文件名
  -z           输出用 gzip 压缩
  -f           前台跑
  -d           在前一个任务后跑
  -D <id,...>  在指定 ID 后跑
  -W <id,...>  在指定 ID 成功后跑
  -L [label]   给任务贴个标签
  -N [num]     需要的槽位数（默认：1）
```

## 致谢

- Андрей Пантюхин (Andrew Pantyukhin) 维护 BSD 移植版
- Alessandro Öhler 提供原始 Gentoo ebuild
- Alexander V. Inyukhin 维护非官方 Debian 包
- Pascal Bleser 为 SuSE/openSuSE 打包
- Gnomeye 维护 AUR 包
- Eric Keller 为 task spooler 队列编写了 nodejs web 服务器
- Duc Nguyen 开发了 GPU 支持
- **Kylin JIANG** 将 Task Spooler 彻底改造为 Task Spooler PLUS：多用户中心化架构、SQLite3 WAL 崩溃恢复、cgroups v1/v2（CPU 限制、freezer、cpuset NUMA 绑定）、NUMA 感知 CPU 绑定分配器与自动碎片整理、超时自动暂停、动态用户管理与热加载、客户端自动重连、定时执行（`--at`）、任务完成回调（`--on-finish`）、PID 反查，以及数百项稳定性修复

## 许可证

详见项目中的 `COPYING` 文件。
