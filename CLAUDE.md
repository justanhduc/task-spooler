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

CPU binding (`TS_CPU_BIND`): requires `hwloc-calc` (from hwloc) to generate topology headers.

**Installing hwloc**:
```bash
# Debian/Ubuntu
sudo apt install hwloc

# RHEL/CentOS/Fedora
sudo dnf install hwloc

# Verify
hwloc-calc --version
```

**Topology detection** — `gen_topology.py` issues these hwloc-calc queries:

```bash
# 1. Count objects of each type
hwloc-calc --number-of NUMANode all    # → NUM_NODES
hwloc-calc --number-of L3Cache all     # → by_l3 count
hwloc-calc --number-of L2Cache all     # → by_l2 count
hwloc-calc --number-of L1Cache all     # → by_l1 count
hwloc-calc --number-of Core all        # → total cores

# 2. For each object i, find which cores it contains (top-down: type → Core)
hwloc-calc NUMANode:0 --intersect Core  # → cores on node 0
hwloc-calc L3Cache:0  --intersect Core  # → cores in L3 0
hwloc-calc L2Cache:0  --intersect Core  # → cores in L2 0
hwloc-calc L1Cache:0  --intersect Core  # → cores in L1 0

# 3. For each core, find its physical PUs (top-down: Core → PU)
hwloc-calc Core:0 --intersect PU --physical  # → physical PUs on core 0
```

The `--physical` flag on PU queries excludes Hyper-Threading siblings (HT excluded by default; pass `--ht` to include them). The script precomputes `core_pu_map` (core→PUs) and `core_node_map` (core→NUMA node) once, then each strategy builds groups by intersecting `type:i` with `Core` and expanding through the cache.

**Generating topology headers**:
```bash
python3 tools/gen_topology.py          # generate all non-trivial strategies
python3 tools/gen_topology.py by_l2    # generate a specific strategy
python3 tools/gen_topology.py --ht     # include HT siblings
cp topology_by_l2.h topology.h         # pick one and link
make TS_CPU_BIND=1                     # build with CPU binding
make TS_CPU_BIND=1 CGROUP_V2=1        # CPU binding + cgroups v2
```

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
#TS_SLOTS = 16         # global default (can be overridden per-user)
john    4
mary    2
```

### CPU Binding (TS_CPU_BIND)

Optional compile-time feature activated with `make TS_CPU_BIND=1`.

1. **tools/gen_topology.py** — runs `hwloc-calc`, detects NUMA nodes, L2/L3/L1 caches, and PUs. Generates `topology_{strategy}.h` with `#define` constants (`NUM_NODES`, `NUM_GROUPS`, `MAX_CORES_PER_GROUP`, `MAX_OS_CPU`, `MAX_GROUPS_PER_NODE`) + `TOPOLOGY_INIT` macro. Handles HT exclusion/inclusion. Skips trivial strategies (1:1 object-to-core mapping) and duplicate topologies. Auto-selects best strategy by average cores/group closest to 4.
2. **cpu_bind.c** — allocator: best-fit group selection, cross-node merge, defrag. Tracks CPU ownership via `cpu_owner[]` array using real job IDs.
3. **`cpu_bind_defrag()`** — two-phase NUMA-aware defrag triggered after each job finishes:
   - Phase 1-2: freeze quality>0 jobs, rebuild allocation state, restore quality=0 allocs
   - Phase 3: try same-node reassignment first (NUMA affinity preserved)
   - Phase 4: cross-node merge for remaining jobs
   - `primary_node` / `mem_nodes` are preserved across defrag cycles
4. **cgroups_set_cpuset()** — writes cpuset.cpus/mems to the cgroup (v1: `/sys/fs/cgroup/cpuset/`, v2: unified hierarchy).
5. **Restart recovery** — `cgroups_restore_all_cpu_bind()` scans cgroup directories, reads cpuset.cpus, rebuilds alloc tracking via `cpu_bind_claim()`.

Ordering: cpuset cgroup is created BEFORE freezer cgroup, so the child process's `cgroups_freeze_ok()` wait covers all cgroups.

Runtime controls: `--no-bind` disables binding per-job (via NEWJOB message) or server-wide (via `cpu_bind_set_disabled()`).

### Cgroups support

Build with `make CGROUP_V2=1` for cgroups v2, or just `make` for v1 (default). Path constants defined at top of `cgroups.c`:
- v1: `/sys/fs/cgroup/cpu/` + `/sys/fs/cgroup/freezer/`
- v2: `/sys/fs/cgroup/` (unified hierarchy, `cpu.max` + `cgroup.freeze`)

### Environment variables

Key overrides: `TS_SOCKET`, `TS_SLOTS`, `TS_USER_PATH`, `TS_LOGFILE_PATH`, `TS_SQLITE_PATH`, `TS_MAXFINISHED`, `TS_MAX_WALL_TIME`, `TS_SORTJOBS`, `TS_SAVELIST`, `TS_ONFINISH`.

## Current branch work

The `cpu-only` branch focuses on CPU binding and cgroups refinements:
- **`cpu_bind_defrag()`** — two-phase NUMA-aware defrag with direct cgroups integration (callback pattern removed)
- **`gen_topology.py`** — multi-strategy output with `MAX_GROUPS_PER_NODE`, skips trivial/duplicate topologies
- **`cpu_owner[]`** tracks real job IDs throughout allocation and defrag
- **`cpu_bind.h`** — `group_ids[]` uses `MAX_GROUPS_PER_NODE`, cleaned up unused typedefs
