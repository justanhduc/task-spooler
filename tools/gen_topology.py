#!/usr/bin/env python3
"""
gen_topology.py — Generate topology.h via hwloc-calc (zero XML parsing).

Queries hwloc-calc CLI for topology — no XML parsing needed.
Precomputes core→PU and core→NUMA maps once at startup,
then all strategies share the cached data (no per-core subprocess calls).

Usage:
    python3 gen_topology.py              # auto-select best strategy
    python3 gen_topology.py by_l2        # specify strategy
    python3 gen_topology.py by_l2 --ht   # include HT siblings

Output: topology.h — #defines + TOPOLOGY_INIT macro for cpu_bind.
"""

import subprocess, sys

MAX_GROUPS = 128
OUTPUT_HEADER = "topology.h"

STRATEGIES = {
    "by_numa": "NUMANode",
    "by_l3":   "L3Cache",
    "by_l2":   "L2Cache",
    "by_l1":   "L1Cache",
    "by_core": "Core",
}


# ── hwloc-calc helpers ──────────────────────────────────────────────────────

def hwloc(args):
    """Run hwloc-calc, return stripped stdout. Exits on failure."""
    try:
        r = subprocess.run(
            ["hwloc-calc"] + args,
            capture_output=True, text=True, check=True, timeout=10,
        )
    except FileNotFoundError:
        print("ERROR: hwloc-calc not found. Install hwloc:\n  apt install hwloc")
        sys.exit(1)
    except subprocess.CalledProcessError as e:
        print(f"ERROR: hwloc-calc failed: {e.stderr.strip() if e.stderr else e}")
        sys.exit(1)
    return r.stdout.strip()


def hwloc_count(obj_type):
    """Return integer count of objects of the given type."""
    return int(hwloc(["--number-of", obj_type, "all"]))


def hwloc_intersect(obj_spec, target_type):
    """Return list of ints from intersecting obj_spec with target_type."""
    out = hwloc([obj_spec, "--intersect", target_type])
    if not out:
        return []
    return [int(x) for x in out.split(",")]


# ── Precomputed topology cache ──────────────────────────────────────────────

def build_topology_cache():
    """Query hwloc-calc once per core to build PU and NUMA lookup tables.

    Returns (core_pu_map, core_node_map) where:
        core_pu_map[i]   = [sorted PU indices for core i]
        core_node_map[i] = NUMA node index for core i
    """
    total_cores = hwloc_count("Core")
    print(f"  Building topology cache for {total_cores} cores ...", end=" ", flush=True)

    core_pu_map = {}
    core_node_map = {}
    for i in range(total_cores):
        core_pu_map[i] = sorted(hwloc_intersect(f"Core:{i}", "PU"))
        node = hwloc_intersect(f"Core:{i}", "NUMANode")
        core_node_map[i] = node[0] if node else 0

    print("done.")
    return core_pu_map, core_node_map


# ── PU collection (from cache) ──────────────────────────────────────────────

def collect_pus(core_indices, core_pu_map, include_ht):
    """Given core OS indices and the precomputed core→PU map, return sorted PU list.

    If include_ht is False, keep only the smallest PU per core (no HT siblings).
    """
    pus = []
    for core_idx in core_indices:
        core_pus = core_pu_map.get(core_idx, [])
        if core_pus:
            if include_ht:
                pus.extend(core_pus)
            else:
                pus.append(min(core_pus))
    return sorted(pus)


def get_node(core_indices, core_node_map):
    """Determine NUMA node from the first core in the list."""
    if not core_indices:
        return 0
    return core_node_map.get(core_indices[0], 0)


# ── Group building ──────────────────────────────────────────────────────────

def build_groups(strategy, core_pu_map, core_node_map, include_ht):
    """Return list of {node, pus} dicts for the given strategy."""
    hwloc_type = STRATEGIES[strategy]
    count = hwloc_count(hwloc_type)
    if count == 0:
        return None

    groups = []
    for i in range(count):
        cores = hwloc_intersect(f"{hwloc_type}:{i}", "Core")
        pus = collect_pus(cores, core_pu_map, include_ht)
        if not pus:
            continue

        node = get_node(cores, core_node_map) if strategy != "by_numa" else i
        groups.append({"node": node, "pus": pus})

    return groups or None


# ── Auto-select ─────────────────────────────────────────────────────────────

def report_strategies(core_pu_map, core_node_map, include_ht):
    """Try all strategies, return (best_strategy_name, best_groups)."""
    TARGET_AVG = 4
    results = []
    for s in STRATEGIES:
        groups = build_groups(s, core_pu_map, core_node_map, include_ht)
        if groups is None:
            results.append((s, None))
            continue
        ng = len(groups)
        mp = min(len(g["pus"]) for g in groups)
        xp = max(len(g["pus"]) for g in groups)
        tp = sum(len(g["pus"]) for g in groups)
        results.append((s, groups, ng, mp, xp, tp))

    # Pick strategy with average cores/group closest to TARGET_AVG (>1 required)
    best, best_dist = None, None
    for r in results:
        s, data, ng, _mp, _xp, tp = r
        if data is None or ng <= 1:
            continue
        dist = abs(tp / ng - TARGET_AVG)
        if best is None or dist < best_dist:
            best, best_dist = s, dist

    best_groups = None
    if best is None:
        for r in results:
            if r[1] is not None:
                best, best_groups = r[0], r[1]
                break
    else:
        for r in results:
            if r[0] == best:
                best_groups = r[1]
                break

    ht_lbl = " (+HT)" if include_ht else ""
    print(f"  {'Strategy':<14} {'Groups':<8} {'PUs/group':<12} {'Note'}")
    print(f"  {'-'*14} {'-'*8} {'-'*12} {'-'*40}")
    for r in results:
        s = r[0]
        if r[1] is None:
            print(f"  {s:<14} {'—':<8} {'—':<12}  (not available)")
            continue
        ng, mp, xp = r[2], r[3], r[4]
        note = (" ← auto-selected" if s == best
                else " (too coarse)" if ng <= 1
                else "")
        n2 = f"{mp}" if mp == xp else f"{mp}–{xp}"
        print(f"  {s:<14} {ng:<8} {n2:<12} {note}{ht_lbl}")

    return best, best_groups


# ── topology.h generation ───────────────────────────────────────────────────

def generate(groups, strategy):
    ng = len(groups)
    nn = max(g["node"] for g in groups) + 1 if groups else 1

    if ng > MAX_GROUPS:
        print(f"ERROR: {ng} groups > MAX_GROUPS={MAX_GROUPS}")
        sys.exit(1)

    max_cpg = max(len(g["pus"]) for g in groups)
    all_pu = set(p for g in groups for p in g["pus"])
    max_os = max(all_pu) + 1 if all_pu else 1

    ng_by_node = {ni: [] for ni in range(nn)}
    for gi, g in enumerate(groups):
        ng_by_node[g["node"]].append(gi)

    g_inits = []
    for gi, g in enumerate(groups):
        cpus = g["pus"]
        nc = len(cpus)
        cl = ", ".join(str(p) for p in cpus)
        pad = max_cpg - nc
        if pad > 0:
            cl += ", " + ", ".join(["0"] * pad)
        g_inits.append(
            f"        [{gi}] = {{.group_id={gi}, .node_id={g['node']},"
            f" .num_cores={nc}, .free_count={nc},"
            f" .os_cpus={{{cl}}}}}"
        )

    n_inits = []
    for ni in range(nn):
        gids = ng_by_node[ni]
        tc = sum(len(groups[gid]["pus"]) for gid in gids)
        gl = ", ".join(str(g) for g in gids)
        n_inits.append(
            f"        [{ni}] = {{.node_id={ni}, .num_groups={len(gids)},"
            f" .num_cores={tc}, .free_count={tc},"
            f" .group_ids={{{gl}}}}}"
        )

    macro = [
        "#define TOPOLOGY_INIT \\",
        "    { \\",
        f"        .num_nodes = {nn}, \\",
        f"        .num_groups = {ng}, \\",
        "        .groups = { \\",
    ]
    for i, gi in enumerate(g_inits):
        comma = "," if i < len(g_inits) - 1 else ""
        macro.append(f"{gi}{comma} \\")
    macro.append("        }, \\")
    macro.append("        .nodes = { \\")
    for i, ni in enumerate(n_inits):
        comma = "," if i < len(n_inits) - 1 else ""
        macro.append(f"{ni}{comma} \\")
    macro.append("        } \\")
    macro.append("    }")

    lines = [
        "/* topology.h — AUTO-GENERATED by gen_topology.py. DO NOT EDIT. */",
        "/* Strategy: " + strategy + " */",
        "",
        "#ifndef TOPOLOGY_H",
        "#define TOPOLOGY_H",
        "",
        "#define NUM_NODES            " + str(nn),
        "#define NUM_GROUPS           " + str(ng),
        "#define MAX_CORES_PER_GROUP  " + str(max_cpg),
        "#define MAX_OS_CPU           " + str(max_os),
        "",
    ] + macro + [
        "",
        "#endif /* TOPOLOGY_H */",
        "",
    ]

    with open(OUTPUT_HEADER, "w") as f:
        f.write("\n".join(lines) + "\n")

    total_pu = len(all_pu)
    print(f"\n\u2713 {OUTPUT_HEADER} generated  (strategy: {strategy})")
    print(f"  NUMA nodes: {nn},  Groups: {ng},  PUs: {total_pu}")
    print(f"\n  {'Group':<8} {'Node':<6} {'PUs':<6}  CPU list")
    print(f"  {'-'*8} {'-'*6} {'-'*6}  {'-'*30}")
    for gi, g in enumerate(groups):
        print(f"  {gi:<8} {g['node']:<6} {len(g['pus']):<6}  "
              f"{', '.join(str(p) for p in g['pus'])}")
    for ni in range(nn):
        gids = ng_by_node[ni]
        all_np = sorted(set(p for gid in gids for p in groups[gid]["pus"]))
        print(f"  Node {ni}: {len(gids)} groups, {len(all_np)} PUs — {all_np}")


# ── Main ────────────────────────────────────────────────────────────────────

def main():
    include_ht = "--ht" in sys.argv
    args = [a for a in sys.argv[1:] if a != "--ht"]
    strategy = args[0].lower() if args else None

    if strategy and strategy not in STRATEGIES:
        print(f"ERROR: unknown strategy '{strategy}'")
        print(f"  Valid: {', '.join(STRATEGIES)} [--ht]")
        sys.exit(1)

    total_pus = hwloc_count("PU")
    total_cores = hwloc_count("Core")
    total_nodes = hwloc_count("NUMANode")
    print(f"  System — {total_pus} PUs, {total_cores} cores, {total_nodes} NUMA nodes"
          f"  {'(+HT)' if include_ht else '(HT excluded)'}")

    # Precompute once — all strategies share this cache
    core_pu_map, core_node_map = build_topology_cache()
    print()

    if strategy:
        groups = build_groups(strategy, core_pu_map, core_node_map, include_ht)
        if groups is None:
            print(f"  '{strategy}' not available.")
            sys.exit(1)
        print(f"  Using: {strategy} ({len(groups)} groups)")
        generate(groups, f"{strategy}{'_ht' if include_ht else ''}")
    else:
        best, groups = report_strategies(core_pu_map, core_node_map, include_ht)
        if groups is None:
            print("ERROR: no valid strategy found.")
            sys.exit(1)
        print(f"\n  Auto-selected: {best}")
        generate(groups, f"{best}{'_ht' if include_ht else ''}")


if __name__ == "__main__":
    main()
