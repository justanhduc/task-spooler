# Changelog

All notable changes to task-spooler-PLUS.

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
