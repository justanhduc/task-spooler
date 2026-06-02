# Changelog

All notable changes to task-spooler-PLUS.

## [v2.5.1] — 2025-Q3

### Changed
- Unified option naming: all long options use dashes (`--get-label`, `--add-wtime`, etc.)
- Server startup output: `key : value` format, PID shown, `[ENV_VAR]` retained
- Help text format: consistent `[param]` style, comma-separated short options
- Removed `pip install hwloc` reference (uses `lstopo` directly)

### Added
- `ts-guardian.sh` / `ts-guardian.conf` — non-system process freezer daemon
- `login-check.sh` — user session audit tool

### Fixed
- CLAUDE.md: restored missing Cgroups support section, corrected user.txt example

## [v2.5] — 2025-Q3

### Added
- **CPU binding allocator** (`TS_CPU_BIND` compile switch)
  - NUMA-aware topology-based CPU allocation
  - cgroups cpuset v1/v2 integration
  - HT exclusion (default) or inclusion (`--ht`)
  - Restart recovery from cgroup filesystem
  - `gen_topology.py` — auto-detect system topology via `lstopo`
  - `--no-bind` — disable CPU binding per-job or server-wide
- **`upgrade_db.py`** — SQLite schema upgrade tool for older databases
- **Human-readable memory** in `ts -i` (auto KB/MB/GB)

### Changed
- `cgroups_create_job()` uses `num_slots` directly instead of `num_allocated`
- cpuset cgroup created before freezer cgroup (correct init order)
- `ts -i` shows `CPU: free` for jobs without binding
- Memory display in `ts -i` now auto-scales (KB/MB/GB)
- PROTOCOL_VERSION bumped to 732 (`no_cpu_binding` field in NEWJOB)

## [v2.4] — 2025-Q2

### Added
- **`--at` scheduled execution**: delay jobs with `+5m`, `14:00`, `2025-06-01T14:00`
  - Client sends schedule_time, server persists in SQLite
  - `ts -l` shows `Wait` state and `Run at ...` output for scheduled jobs
  - `ts -i` shows `Schedule:` line with time and remaining duration
- **Client auto-reconnect on crash**: queued and running jobs survive server restart
  - Client reconnects every 5s using RECONNECT protocol
  - `s_add_job()` preserves QUEUED/RUNNING state (no fork, no DELINK)

### Changed
- Server `select()` uses 1s timeout for periodic schedule checks
- List display hides `(-nan%)` for jobs with zero runtime
- `ts -i` hides work/pause/elapsed time when value is zero

## [v2.3] — 2025-Q1

### Added
- **`--on-finish` per-job callback**: run a command after job finishes with full job info via placeholders
  - Placeholders: `{jobid}`, `{output}`, `{exitcode}`, `{pid}`, `{label}`, `{command}`, `{realtime}`, `{usertime}`, `{systime}`, `{pausetime}`, `{start_time}`, `{enque_time}`, `{end_time}`, `{slots}`
  - `{output}`, `{label}`, `{command}` auto-quoted for shell safety
  - `ENDJOB_OK` protocol: server sends final timing data (pause duration, start/enqueue/end time) after job finishes
  - Client-side execution (user context, not root)
- **Client auto-reconnect**: on server crash, client retries connection automatically
  - `RECONNECT`/`RECONNECT_OK` protocol for seamless re-attachment
  - First retry immediately, then every 60s
  - Works for both running and finished jobs
- **Orphan child cleanup**: server kills orphaned subprocesses when client disconnects during RUNNING/PAUSE state

### Removed
- **Server-side `--relink` mechanism**: replaced by client auto-reconnect
  - Removed `RELINK` state, `s_check_relink()`, `run_relink()`, `ptrace_pid()`, `wait_for_pid()`
- **Sound notification**: removed `SOUND` ifdef, `paplay`/PulseAudio defaults

### Changed
- Time calculation moved before job callbacks to provide timing data to `--on-finish`

## [v2.2] — 2024-Q2 (cpu-only branch)

### Added
- **Cgroups v2 support**: unified hierarchy (`cpu.max` + `cgroup.freeze`), compile-time switch (`make CGROUP_V2=1`)
- **User management via `vec_t`**: `struct User` with dynamic array, username-only config (UID resolved by `getpwnam`)
- **PID lookup**: `--find-by-pid` finds which job owns a process (including descendants via `/proc/*/children`)
- **Per-job boot time**: stores boot epoch per-job in SQLite `end_time_ms` column for correct wall-clock display after reboot
- **`REMOVEJOB_NOK` protocol**: proper error response when remove-job fails
- **Refresh safety**: `ts -X` only allows adding users, rejects removals or slot changes
- **Single-instance guard**: `/proc` scan on `--daemon` prevents two root servers
- **SQLite WAL mode**: `PRAGMA journal_mode=WAL` + `busy_timeout=5000`, checkpoint on graceful exit
- **Tools**: `tools/clear_finished.py` (clear/drop Finished table), `tools/migrate_uid.py` (migrate ts_UID to Linux UID)
- **man page**: `ts.1` manual page
- **Test script**: `manual_test.sh` covering all operations with `sleep` commands

### Changed
- **User config format**: `UID name slots` → `name slots` (no manual UID needed)
- **`struct Job.ts_UID`** → `struct User *user` pointer
- **All server functions** take `struct User *` instead of `int ts_UID`
- **`sprintf` → `snprintf`** in sqlite.c global buffer
- **`user_locker`**: `int` index → `struct User *` pointer
- **Cgroups refactor**: shared public interface dispatches to v1 or v2 internally

### Fixed
- PAUSE job `-r`/`-k`: thaw before cleanup/kill so signals can be delivered
- `cgroups_v1_cleanup_cpu`: EBUSY retry + thaw-before-kill ordering
- `new_finished_job`: use `insert_or_replace_DB` to avoid UNIQUE constraint on duplicate finish
- `read_DB`: recover user from `/proc/<pid>/status` for active Jobs table rows
- `read_jobid_DB`/`read_DB`: removed redundant `sqlite3_exec` before `sqlite3_prepare`

## [v1.3.1-cpu] — 2022-06-09

- readme updates
- bumped version
- erroneous free fix
- removed redundancy
- plain list output
- various memory fixes
- local installation support

## [v1.3.0-cpu] — 2021-11-18

- free malloc'ed memory
- more time units in `-i` display
- fixed Makefile
- git version embedding
- setenv/getenv/unsetenv support
- configurable log filename and directory
- changed gzip flag from `-g` to `-z`

## [v1.2.1-cpu] — 2021-10-07

- adaptive command display width
- more responsive command display
- updated readme and man page

## [v1.2-cpu] — 2021-05-28

- killed servers without terminating jobs
- auto-changelog script

## [v1.1.4-cpu] — 2021-02-08

- handled invalid dependencies
- fixed memory leaks

## [v1.1.3-cpu] — 2021-02-06

- multiple dependency support
- cmake support
- improved help

## [v1.1.2-cpu] — 2021-02-04

- dependency exit code check (`-W`)
- updated readme

## [v1.1.1-cpu] — 2020-12-05

- major bug fixes
- show full command
- shortened label/command display
- uninstall script
- various reformats

## v1.1.0-cpu — 2020-12-05

- kill all running
- dependency fix
- improved argument parsing
- fixed time unit
- get last queued job ID
- count running jobs
- initial release (forked from upstream task-spooler)
