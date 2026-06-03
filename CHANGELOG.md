# Changelog

All notable changes to task-spooler-PLUS.

## [v2.6.3] — 2026-Q2

### Changed
- **Jobid init refactored** — robust startup with duplicate prevention
  - `init_jobids_DB()`: validates seed against existing Jobs/Finished tables, bumps if needed
  - `get_jobids_DB()` → valid (>0) → use directly; invalid → full scan + fallback to `TS_FIRST_JOBID` / 1000
  - `set_jobids_DB()`: lightweight runtime write (no scan)
  - `Global` table `INSERT OR IGNORE` to avoid UNIQUE constraint on restart
  - `[set_jobids_DB]` error output now includes return code

### Fixed
- `gen_topology.py`: summary shows recommended strategy alongside generated files

## [v2.6.2] — 2026-Q2

### Changed
- **JSON output** (`-M json`) expanded to match `-i` info fields (times, CPU bind, dependencies, etc.)
- **`-M json -J <id>`** filters JSON output to a single job (reuses existing `--jobid` / `-J`)

### Removed
- **Mail subsystem** — `mail.c`, `mail.h`, `mymail.sh`, ssmtp/sendmail integration removed
  - `-m` CLI option removed; use `--on-finish` hook for notifications
  - `TS_MAIL_FROM`, `TS_MAIL_TIME` env vars removed
  - `TS_ONFINISH` retained as default on-finish command (overridden by `--on-finish`)
- **Legacy `TS_ONFINISH` hook** (`hook_on_finish` wrapper) removed; `--on-finish` is the only callback path

### Fixed
- `run_on_finish`: simplified from `fork+execl("/bin/sh",...)` to `system()`

## [v2.6.1] — 2026-Q2

### Changed
- **`cpu_bind_defrag()` refactored** — two-phase NUMA-aware defrag:
  - Phase 3: reassign jobs on their original `primary_node` first (NUMA affinity preserved)
  - Phase 4: cross-node merge for remaining jobs
  - Callback pattern removed; directly calls `cgroups_freeze_job` / `cgroups_set_cpuset` / `cgroups_thaw_job`
- **`cpu_bind_alloc_init()`** now takes `jobid`; `cpu_owner[]` tracks real job IDs throughout allocation
- **`cpu_bind_post_alloc()`** called in both initial allocation and defrag paths (sort + validate)
- **`gen_topology.py`** — generates `.h` files for ALL non-trivial strategies at once
  - New `#define MAX_GROUPS_PER_NODE` in every output
  - Skips trivial strategies (count == cores, or count == PUs with `--ht`; except `by_core`)
  - Skips duplicate strategies (identical group topologies)

### Fixed
- `cgroups_set_cpuset`: create cgroup v2 directory before writing `cpuset.cpus`
- `cpu_bind_defrag`: memory leak on early return (all quality=0)
- `cpu_bind_defrag`: frozen jobs lost due to `jobs[n]` overwrite bug

### Removed
- `cpu_bind_pause_fn` / `cpu_bind_update_fn` / `cpu_bind_resume_fn` typedefs (no longer used)
- `defrag_pause_job` / `defrag_update_cpuset` / `defrag_resume_job` callbacks in `jobs.c`

## [v2.6.0] — 2026-Q2

### Changed
- **`cpu_bind_defrag()`** auto-triggered after each job finishes
- **`gen_topology.py` rewritten**: uses `hwloc-calc` CLI instead of parsing `lstopo` XML
  - All topology queries follow top-down hierarchy (`type:i → Core → PU`)
  - Precomputed `core_pu_map` and `core_node_map` at startup, shared across all strategies
  - `build_groups` skips `hwloc-calc` when count == total_cores (by_core or 1:1 cache mapping)
  - Auto-select targets average cores/group closest to 4 (tie-break: `by_l2 → by_core → by_l1 → by_l3 → by_numa`)
  - MAX_GROUPS raised from 32 to 128

### Fixed
- `main.c`: missing newline in `TS_MAXCONN` help output

### Removed
- `upgrade_db.py` — no longer needed

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
