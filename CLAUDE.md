# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build & Run

```bash
make                      # Build `ts` binary (cgroups v1, default)
make CGROUP_V2=1          # Build with cgroups v2 support
make clean                # Remove objects and binary
./install_make            # Install to /usr/local (needs root)
./testbench.sh            # Integration test script
```

No configure step needed. No unit test framework — `testbench.sh` exercises the binary end-to-end.

Build uses `-std=gnu11 -Wall -ansi -pedantic -fcommon -Wno-format-truncation`. The `version.h` is generated at build time or created manually; `Makefile` appends git describe output when inside a worktree.

## Architecture

**Client-server over Unix domain socket** (usually `/tmp/socket-ts.root`). The server must run as root (uses `SO_PEERCRED` for auth). The client forks and executes the actual job — the server never runs user commands.

### Process model

```
ts (client)  ──Unix socket──▶  ts (server daemon)
   │                              │
   fork() + exec(cmd)             │  manages queue, slots, users
   │                              │  persists to SQLite3
   waitpid() → notify server      │
```

1. `main.c` — CLI arg parsing (`parse_opts`), dispatches to `c_*` client functions
2. `server_start.c` — socket path creation, auto-launches server if not running via `ensure_server_up()`
3. `server.c` — `server_main()` → `server_loop()`: single-threaded `select()` loop over client connections
4. `client.c` — client-side message send/recv for every action (queue, list, kill, tail, etc.)
5. `jobs.c` — core job queue: `vec_t active_jobs` / `vec_t finished_jobs`, job lifecycle (QUEUED→RUNNING→FINISHED), slot accounting, wall-time timeout check, dependency resolution, cgroups freeze/thaw integration
6. `execute.c` — `run_job()`: fork+exec the user command, capture exit status and timing
7. `sqlite.c` — SQLite3 persistence: `Jobs` table (active queue), `Finished` table (completed), `Global` table (jobid counter). Schema mirrors `struct Job`/`Result`/`Procinfo`
8. `cgroups.c` — cgroups v1/v2: CPU quota limiting and freezer-based pause/resume. V2 selected via `-DCGROUP_V2` compile flag
9. `runtime_limit.c` — wall-time enforcement helpers, `parse_time()` for human durations like "30s", "3.4m", "1.5H", "2d"
10. `user.c` — multi-user config in `vec_t users_vec`, `struct User` with `uid_t uid`, slot mapping, user lock/suspend state

### Key data structures

- **`struct Job`** (`jobs.h`) — all job state: id, command, state, result, output filename, dependencies, wall-time, cgroup info, `struct User *user` pointer
- **`struct User`** (`user.h`) — user state: name, uid, max_slots, busy/jobs/queue counters, locked flag. Stored in dynamic `vec_t users_vec`
- **`struct Msg`** (`msg.h`) — wire protocol message with union of all request/response types; type-tagged binary over Unix socket
- **`struct Client_conn`** (`server.h`) — per-connection state: socket fd, hasjob flag, jobid, `struct User *user`
- **`struct CommandLine`** (`main.h`) — parsed CLI state, passed through to client functions

### Job state machine

```
QUEUED → RUNNING → FINISHED
  ↓         ↓
LOCKED    PAUSE (freezer cgroup)
  ↓         ↓
QUEUED    RUNNING (on resume/cont)

Special states: HOLDING_CLIENT (queue full), RELINK (crash recovery), DELINK/WAIT (SQLite restored)
```

### User auth & slots

The server reads a user config file (path from `TS_USER_PATH` or env var) that maps usernames to max slot counts. Usernames are resolved to UIDs via `getpwnam()`. Users are stored in a dynamic `vec_t users_vec` with `struct User` entries. `SO_PEERCRED` on the Unix socket identifies the caller — the server looks up the peer's UID in the user vec. Root (UID 0) has full control. Each user gets a slot budget; jobs specify required slots via `-N`.

**user.txt format**:
```
# <username> <max_slots>
TS_SLOTS = 16
john    4
mary    2
```

### Cgroups support

Build with `make CGROUP_V2=1` for cgroups v2, or just `make` for v1 (default). Path constants defined at top of `cgroups.c`:
- v1: `/sys/fs/cgroup/cpu/` + `/sys/fs/cgroup/freezer/`
- v2: `/sys/fs/cgroup/` (unified hierarchy, `cpu.max` + `cgroup.freeze`)

### Environment variables

Key overrides: `TS_SOCKET`, `TS_SLOTS`, `TS_USER_PATH`, `TS_LOGFILE_PATH`, `TS_SQLITE_PATH`, `TS_MAXFINISHED`, `TS_MAX_WALL_TIME`, `TS_SORTJOBS`, `TS_SAVELIST`, `TS_ONFINISH`.

## Current branch work

The `cpu-only` branch has: cgroups v1+v2 CPU/ freezer, `struct User`-based user management with `vec_t`, per-job `boot_time` in SQLite, PID lookup (`--find-by-pid`), and `REMOVEJOB_NOK` error protocol.
