# Task Spooler PLUS

This project builds upon [Task Spooler by Lluís Batlle i Rossell](https://vicerveza.homeunix.net/~viric/soft/ts/), enhanced with **multi-user support, SQLite-based crash recovery, cgroups CPU limiting and freeze/pause, and dynamic user management**. It runs as a root-privileged background service and is designed for workstations shared by several to tens of users.

## Introduction

As a computer scientist, I frequently submit multiple simulation tasks on shared workstations. The original task-spooler lacked multi-user support — each user maintained an independent queue. <u>task-spooler-PLUS</u> adds a centralised server that manages all users through a single queue.

Recent enhancements include: **cgroups v1/v2 CPU limiting and freezer-based pause/resume**, **SQLite3 WAL-mode persistence with crash recovery**, and dynamic `struct User` management via `vec_t`.

### Changelog

See [CHANGELOG](CHANGELOG.md).

## Features

- **Cross-platform** task queue for GNU/Linux, Darwin, Cygwin, FreeBSD
- **Multi-user support** with per-user slot limits
- **Fatal crash recovery** via SQLite3 (WAL mode) — jobs, states, and timings survive reboots
- **Cgroups CPU limiting** and **freezer-based pause/resume** (v1 and v2, compile-time selectable)
- **Wall-time management** — auto-pause and re-queue timed-out tasks
- **Global user control** — suspend/resume all jobs for a single user
- **Comprehensive output** in default, JSON, and tab-separated formats
- **Simple build** — just `make` (no autotools)
- **Optional stderr separation** for better log management
- **PID lookup** (`--find-by-pid`) to identify which job owns a process (including descendants)
- **Scheduled execution** (`--at`) — delay jobs until a specified time (+5m, 14:00, 2025-06-01T14:00)
- **CPU binding** (`TS_CPU_BIND`) — NUMA-aware topology-based CPU allocation with cgroups cpuset v1/v2, HT exclusion, and crash-restart recovery
- **Crash survival** — jobs persist through server restart with automatic client reconnect
- **Post-job hook** (`--on-finish`) — run a command after a job finishes, with access to job info via placeholders

## Tools

- `tools/migrate_uid.py` — Migrate old `ts_UID` column (vec index) to Linux UIDs
- `tools/clear_finished.py` — Clear or `--drop` + recreate the Finished table

## Quick Start

```bash
make                      # build (cgroups v1, default)
make CGROUP_V2=1          # build with cgroups v2 support
make TS_CPU_BIND=1        # build with CPU binding (cgroups cpuset)
make CGROUP_V2=1 TS_CPU_BIND=1  # build with both cgroups v2 + CPU binding
sudo ./ts --daemon        # start the server as daemon (root only)
./ts -l                   # list jobs
./ts sleep 30             # enqueue a job
./ts -r <id>              # remove a job
./ts -k <id>              # kill a running job
./ts -w <id>              # wait for a job to finish
```

## Build & Install

```bash
make                      # Build `ts` binary
make CGROUP_V2=1          # Build with cgroups v2
make clean                # Remove objects and binary
./install_make            # Install to /usr/local (needs root)
```

**Default paths** (override via environment variables):

| Variable | Default | Purpose |
|----------|---------|---------|
| `TS_SOCKET` | `$TMPDIR/socket-ts.root` | Unix socket |
| `TS_USER_PATH` | `/home/kylin/task-spooler/user.txt` | User config |
| `TS_LOGFILE_PATH` | `/home/kylin/task-spooler/log.txt` | Job log |
| `TS_SQLITE_PATH` | `/home/kylin/task-spooler/task-spooler.db` | SQLite database |
| `TS_SLOTS` | `1` | Max concurrent jobs |
| `TS_MAXFINISHED` | `1000` | Max finished jobs |
| `TS_MAX_WALL_TIME` | `10080` (minutes) | Max wall-time limit |
| `TS_FIRST_JOBID` | `1000` | Starting job ID |

Edit `defaults.h` to change the built-in defaults.

## User Configuration

The server reads a user config file (path from `TS_USER_PATH`) mapping **usernames** to **max slot counts**. Usernames are resolved to Linux UIDs via `getpwnam()` — no need to manually specify UIDs.

**user.txt** format:

```
# <username> <max_slots>
TS_SLOTS = 16
john    4
mary    2
```

Root (uid=0) is automatically added with full control.

The config can be refreshed at runtime via `ts -X` (root only). The refresh only allows **adding** new users — existing users cannot be removed or have their slots changed.

## How it works

A **server process** runs as root, managing jobs in memory and persisting state to SQLite3. **Client processes** connect via a Unix socket. The server never executes user commands — clients fork and run jobs themselves, preserving the user's environment, ulimits, and working directory.

```
ts (client)  ──Unix socket──▶  ts (server daemon)
   │                              │
   fork() + exec(cmd)             │  manages queue, slots, users
   │                              │  persists to SQLite3 (WAL)
   waitpid() → notify server      │
```

On crash, running jobs are re-attached via `--relink`. On reboot, all job state is restored from SQLite.

### Cgroups support

Build-time selection:
- `make` — cgroups v1 (`cpu.cfs_quota_us` + `freezer.state`)
- `make CGROUP_V2=1` — cgroups v2 (`cpu.max` + `cgroup.freeze`)

Both provide CPU quota limiting and freezer-based pause/resume.

To check which cgroup version your system supports:
```bash
mount | grep cgroup
# v1 shows: cgroup on /sys/fs/cgroup/cpu, freezer, cpuset ...
# v2 shows: cgroup2 on /sys/fs/cgroup type cgroup2
```

### Single-instance guard

The server checks `/proc` on `--daemon` startup: if another instance of the same binary is already running as root, it refuses to start.

### Common problems

- **Server stuck**: remove the socket file (`/tmp/socket-ts.root`), then restart
- **After SIGKILL**: `.db-wal` and `.db-shm` files persist — SQLite auto-recovers on next open
- **After crash**: running jobs lose exit code and signal information

## Manual

See `man ts` or run `ts -h` for the full command reference.

```
Task Spooler 2.1.1a - a task queue system for the unix user.
Copyright (C) 2007-2024  Kylin JIANG - Duc Nguyen - Lluis Batlle i Rossell

Environment Variables:
  TS_SOCKET        : Unix socket path (default: $TMPDIR/socket-ts.root)
  TS_SLOTS         : Max concurrent jobs (server start, default: 1)
  TS_USER_PATH     : User config file path (server start)
  TS_LOGFILE_PATH  : Job log path (server start)
  TS_SQLITE_PATH   : SQLite DB path (server start)
  TS_MAXFINISHED   : Max finished jobs (default: 1000)
  TS_MAX_WALL_TIME : Max wall-time (default: 10080 min)
  TS_MAXCONN       : Max connections (default: 1000)
  TS_SORTJOBS      : Job queue sorting control
  TS_SAVELIST      : Crash recovery file for job list
  TS_ONFINISH      : Binary executed post-job
  TS_ENV           : Command to gather job info during enqueue
  TS_MAIL_FROM     : Sender email for results
  TS_MAIL_TIME     : Email threshold in seconds
  TMPDIR           : Temporary output directory

Long option actions:
  --getenv [var]          Get server environment variable
  --setenv [var]          Set server environment flag
  --unsetenv [var]        Remove server environment flag
  --get-label || -a [id]  Show job label
  --full-cmd || -F [id]   Show full command
  --find-by-pid [pid]     Find which running job a PID belongs to
  --check-daemon           Verify daemon status
  --count-running || -R   Count running jobs
  --last-queue-id || -q   Show last added job ID
  --get-logdir             Display log directory path
  --set-logdir [path]     Configure log directory
  --serialize || -M [fmt] Export job list (default/json/tab)
  --hold [jobid]          Pause specified job
  --cont [jobid]          Resume paused job
  --suspend [USER]        Suspend user
  --resume [USER]         Resume user
  --lock                  Lock server
  --unlock                Release server lock
  --at <time>             Schedule: +5m, 14:00, 06-01_14:00, 2025-06-01T14:00
  --on-finish <template>  Run command after job finishes
                          Placeholders: {jobid} {output} {exitcode} {pid} {label}
                          {command} {realtime} {usertime} {systime}
                          {pausetime} {start_time} {enque_time} {end_time} {slots}
  --wtime [dur]           Wall time limit (e.g. 30s, 3.4m, 1.5H, 2d)
  --add-wtime [dur]       Increase job wall time (root only)
  --job [id] || -J [id]  Specify job ID
  --daemon                Run as daemon (root only)

Actions:
  -A           List info for all users
  -X           Refresh user config (root only)
  -K           Stop server (root only)
  -C           Clear finished jobs
  -l           Show job list (default)
  -S [num]     Get/set max concurrent jobs (root only)
  -t [id]      Tail last 10 lines of output
  -c [id]      Show complete output
  -p [id]      Display job PID
  -o [id]      Show output file path
  -i [id]      Display job info
  -s [id]      Show job state
  -r [id]      Remove job
  -w [id]      Wait for job
  -k [id]      Send SIGTERM to job
  -T           SIGTERM all jobs (root only)
  -u [id]      Prioritize job
  -U <id-id>   Swap two jobs
  -h           Show help
  -V           Display version

Options adding jobs:
  -B           Exit if server full
  -n           Disable output storage
  -E           Separate stderr
  -O           Set log filename
  -z           Gzip output
  -f           Run in foreground
  -m <email>   Email results
  -d           Run after last job
  -D <id,...>  Run after specified IDs
  -W <id,...>  Run after successful IDs
  -L [label]   Assign job label
  -N [num]     Required slots (default: 1)
```

## History

- Андрей Пантюхин (Andrew Pantyukhin) maintains the BSD port.
- Alessandro Öhler provided the original Gentoo ebuild.
- Alexander V. Inyukhin maintains unofficial Debian packages.
- Pascal Bleser packed the program for SuSE/openSuSE.
- Gnomeye maintains the AUR package.
- Eric Keller wrote a nodejs web server for the task spooler queue.
- Duc Nguyen developed GPU support.
- **Kylin JIANG** added: multi-user support, SQLite3 crash recovery, cgroups CPU/freezer (v1+v2), dynamic user management, PID lookup, and numerous stability fixes.

## License

See the provided `COPYING` file.
