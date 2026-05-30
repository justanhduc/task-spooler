# Task Spooler PLUS

This project builds upon [Task Spooler by Lluís Batlle i Rossell](https://vicerveza.homeunix.net/~viric/soft/ts/), enhanced with **multi-user support, crash recovery via SQLite3, and cgroups-based resource limiting (v1 & v2)**. Designed for personal workstations and small shared servers — a lightweight alternative to SLURM or PBS.

## Features

- **Multi-user** with per-user slot limits
- **Crash recovery** via SQLite3 — all jobs (running, queued, finished) persist across restarts
- **Cgroups CPU limiting + freeze/thaw** — v1 and v2, selectable at build time
- **Wall-time management** — auto-pause timed-out jobs, requeue for later execution
- **PID lookup** (`--find-by-pid`) — find which job owns a process (including descendants)
- **Multiple output formats** — default, JSON, tab-separated
- **User refresh validation** — refresh only adds users, never removes
- **Single-instance guard** — /proc scan prevents dual server instances

## Quick Start

```bash
make                      # Build (cgroups v1, default)
make CGROUP_V2=1          # Build with cgroups v2
./install_make            # Install to /usr/local (needs root)
```

The server runs as **root**. Clients connect via Unix socket (`/tmp/socket-ts.root` by default).

## User Configuration

Format: `<username> <max_slots>` — username must exist on the system (UID resolved via `getpwnam`).

```
# user.txt (set via TS_USER_PATH env var)
TS_SLOTS = 16
john    4
mary    2
```

## Environment Variables

| Variable | Purpose | Default |
|----------|---------|---------|
| `TS_SOCKET` | Unix socket path | `$TMPDIR/socket-ts.root` |
| `TS_SLOTS` | Max concurrent jobs | `1` |
| `TS_USER_PATH` | User config file | `user.txt` |
| `TS_SQLITE_PATH` | SQLite DB path | `task-spooler.db` |
| `TS_LOGFILE_PATH` | Job log path | `log.txt` |
| `TS_MAXFINISHED` | Max finished jobs in memory | `1000` |
| `TS_MAX_WALL_TIME` | Max wall-time (seconds) | `604800` (7 days) |
| `TS_MAXCONN` | Max connections | `1000` |
| `TS_SORTJOBS` | Sort control | `0` |
| `TS_SAVELIST` | Crash recovery dump file | (none) |
| `TS_FIRST_JOBID` | Initial job ID | `1000` |
| `TMPDIR` | Temp output directory | `/tmp` |

## Build Options

```bash
make                      # cgroups v1 (default)
make CGROUP_V2=1          # cgroups v2 — unified /sys/fs/cgroup/
```

## Architecture

**Client-server over Unix socket.** The server manages the queue; clients fork and execute jobs.

```
ts (client)  ──Unix socket──▶  ts (server)
   │                              │
   fork() + exec(cmd)             │  queue, slots, users
   │                              │  SQLite3 persistence
   waitpid() ➔ notify server      │  cgroups CPU + freezer
```

### Key files

| File | Purpose |
|------|---------|
| `main.c` | CLI parsing, dispatch |
| `server.c` | `select()` event loop |
| `jobs.c` | Job queue, state machine, slot accounting |
| `execute.c` | `fork`+`exec`, exit code capture |
| `cgroups.c` | CPU limit + freeze/thaw (v1/v2) |
| `sqlite.c` | SQLite3 WAL-mode persistence |
| `user.c` | `vec_t users_vec` + `struct User` |

## Usage

```
Actions:
  -l           List jobs (default)
  -N [num]     Number of slots required
  -L [label]   Job label
  -d           Depend on last job
  -D <id,...>  Depend on specified jobs
  -w [id]      Wait for job
  -r [id]      Remove job
  -k [id]      SIGTERM job
  -t [id]      Tail output
  -c [id]      Cat output
  -p [id]      Show job PID
  -P <pid>     Find which job owns PID (incl. descendants)
  -i [id]      Job info
  -s [id]      Job state
  -u [id]      Urgent (move to front)
  -U <id-id>   Swap two jobs
  -C           Clear finished jobs
  -S [num]     Get/set max slots (root)
  -K           Kill server (root)
  -X           Refresh user config (root)
  -A           List all users
  -R           Count running
  -q           Last job ID
  -V           Version
  -h           Help
  --daemon     Start server as daemon (root)
  --find-by-pid <pid>  Find job by process PID
  --hold [id]  Pause job
  --cont [id]  Resume job
  --suspend [user]  Suspend user
  --resume [user]   Resume user
  --wtime <t>  Wall-time limit (e.g. 30s, 3.4m, 1.5H, 2d)
  --add_wtime <t>  Add wall-time (root)
  --getenv <var>   Get env
  --setenv <var>   Set env flag
  --unsetenv <var> Remove env flag
  --tmp        Store output in /tmp
  --serialize <fmt>  Export list (json/tab)
```

## History

- **Kylin JIANG** — multi-user support, SQLite crash recovery, cgroups v1/v2
- **Duc Nguyen** — GPU support
- **Lluís Batlle i Rossell** — original task-spooler
