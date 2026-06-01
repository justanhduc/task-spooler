#!/usr/bin/env python3
"""
gen_topology.py — Generate topology.h via lstopo.

Usage:
    python3 gen_topology.py              # auto-select best strategy
    python3 gen_topology.py by_l2        # specify strategy
    python3 gen_topology.py by_l2 --ht   # include HT siblings

Output: topology.h — #defines + TOPOLOGY_INIT macro for cpu_bind.
"""

import subprocess, os, sys, xml.etree.ElementTree as ET

MAX_GROUPS = 32
OUTPUT_HEADER = "topology.h"
ALL_STRATEGIES = ["by_numa", "by_l3", "by_l2", "by_core", "by_l1"]

# ---------------------------------------------------------------------------
# lstopo XML parsing
# ---------------------------------------------------------------------------
def parse_cpuset(hex_str):
    if not hex_str:
        return []
    try:
        mask = int(hex_str, 16)
    except ValueError:
        return []
    cpus, i = [], 0
    while mask:
        if mask & 1:
            cpus.append(i)
        mask >>= 1
        i += 1
    return cpus

def run_lstopo():
    try:
        r = subprocess.run(["lstopo", "--of", "xml"],
                           capture_output=True, text=True, check=True, timeout=10)
    except FileNotFoundError:
        print("ERROR: lstopo not found. Install hwloc:\n  apt install hwloc")
        sys.exit(1)
    except subprocess.CalledProcessError as e:
        print(f"ERROR: lstopo failed: {e}")
        sys.exit(1)
    return ET.fromstring(r.stdout)

def collect_objects(root):
    objs = []
    def walk(el):
        if el.tag != "object":
            for c in el:
                walk(c)
            return
        objs.append({"type": el.get("type"), "os_index": int(el.get("os_index", -1)),
                     "cpus": parse_cpuset(el.get("cpuset", ""))})
        for c in el:
            walk(c)
    walk(root)
    return objs

# ---------------------------------------------------------------------------
# Strategy implementations
# ---------------------------------------------------------------------------
def strategy_groups(strategy, all_objs, include_ht=False):
    numa_nodes = [o for o in all_objs if o["type"] == "NUMANode"]
    all_cores = [o for o in all_objs if o["type"] == "Core"]

    def find_node(cs):
        for ni, n in enumerate(numa_nodes):
            if set(cs).issubset(set(n["cpus"])):
                return ni
        return 0

    def filter_ht(cpus):
        if include_ht:
            return set(cpus)
        result = set()
        for core in all_cores:
            inc = [c for c in core["cpus"] if c in cpus]
            if inc:
                result.add(min(inc))
        return result

    # by_numa
    if strategy == "by_numa":
        groups = []
        for ni, n in enumerate(numa_nodes if numa_nodes else [{"os_index": -1, "cpus": list(range(256))}]):
            pus = sorted(filter_ht(n["cpus"]))
            if pus:
                groups.append({"node": ni if numa_nodes else 0, "pus": pus})
        return groups or None

    # by_l3 / by_l2 / by_l1
    cmap = {"by_l3": "L3Cache", "by_l2": "L2Cache", "by_l1": "L1Cache"}
    if strategy in cmap:
        objs = [o for o in all_objs if o["type"] == cmap[strategy]]
        if not objs:
            return None
        groups = []
        for co in objs:
            pus = sorted(filter_ht(co["cpus"]))
            if pus:
                groups.append({"node": find_node(co["cpus"]), "pus": pus})
        return groups or None

    # by_core
    if strategy == "by_core":
        groups = []
        for core in all_cores:
            pus = sorted(filter_ht(core["cpus"]))
            if pus:
                groups.append({"node": find_node(core["cpus"]), "pus": pus})
        return groups or None
    return None

# ---------------------------------------------------------------------------
# Report & auto-select
# ---------------------------------------------------------------------------
def compute_quality(ng, _, __, total):
    return 0 if ng <= 1 else 1

def report_strategies(all_objs, include_ht=False):
    auto_order = ["by_l2", "by_core", "by_l1", "by_l3", "by_numa"]
    best = None
    results = []

    for s in ALL_STRATEGIES:
        groups = strategy_groups(s, all_objs, include_ht)
        if groups is None:
            results.append((s, None))
            continue
        ng = len(groups)
        mp = min(len(g["pus"]) for g in groups)
        xp = max(len(g["pus"]) for g in groups)
        tp = sum(len(g["pus"]) for g in groups)
        results.append((s, groups, ng, mp, xp, tp))
        if best is None and s in auto_order and compute_quality(ng, mp, xp, tp) > 0:
            best = s
    if best is None:
        for r in results:
            if r[1] is not None:
                best = r[0]; break

    ht_lbl = " (+HT)" if include_ht else ""
    print(f"  {'Strategy':<14} {'Groups':<8} {'PUs/group':<12} {'Note'}")
    print(f"  {'-'*14} {'-'*8} {'-'*12} {'-'*40}")
    for r in results:
        s = r[0]
        if r[1] is None:
            print(f"  {s:<14} {'—':<8} {'—':<12}  (not available)")
            continue
        ng, mp, xp = r[2], r[3], r[4]
        note = " ← auto-selected" if s == best else (" (too coarse)" if compute_quality(ng, mp, xp, r[5]) == 0 else "")
        n2 = f"{mp}" if mp == xp else f"{mp}–{xp}"
        print(f"  {s:<14} {ng:<8} {n2:<12} {note}{ht_lbl}")
    return best

# ---------------------------------------------------------------------------
# Generate topology.h
# ---------------------------------------------------------------------------
def generate(groups, strategy):
    ng = len(groups)
    nn = max(g["node"] for g in groups) + 1 if groups else 1

    if ng > MAX_GROUPS:
        print(f"ERROR: {ng} groups > MAX_GROUPS={MAX_GROUPS}"); sys.exit(1)
    max_cpg = max(len(g["pus"]) for g in groups)
    all_pu = set(p for g in groups for p in g["pus"])
    max_os = max(all_pu) + 1 if all_pu else 1

    ng_by_node = {ni: [] for ni in range(nn)}
    for gi, g in enumerate(groups):
        ng_by_node[g["node"]].append(gi)

    # Build group and node initializer strings
    g_inits = []
    for gi, g in enumerate(groups):
        cpus = g["pus"]
        nc = len(cpus)
        cl = ", ".join(str(p) for p in cpus)
        pad = max_cpg - nc
        if pad > 0:
            cl += ", " + ", ".join(["0"] * pad)
        g_inits.append(f"[{gi}] = {{.group_id={gi}, .node_id={g['node']}, "
                       f".num_cores={nc}, .free_count={nc}, "
                       f".os_cpus={{{cl}}}}}")

    n_inits = []
    for ni in range(nn):
        gids = ng_by_node[ni]
        tc = sum(len(groups[gid]["pus"]) for gid in gids)
        gl = ", ".join(str(g) for g in gids)
        n_inits.append(f"[{ni}] = {{.node_id={ni}, .num_groups={len(gids)}, "
                       f".num_cores={tc}, .free_count={tc}, "
                       f".group_ids={{{gl}}}}}")

    g_inits_str = ", ".join(g_inits)
    n_inits_str = ", ".join(n_inits)
    init_body = f"{{.num_nodes={nn}, .num_groups={ng}, .groups={{{g_inits_str}}}, .nodes={{{n_inits_str}}}}}"

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
        "#define TOPOLOGY_INIT  " + init_body,
        "",
        "#endif /* TOPOLOGY_H */",
        "",
    ]

    with open(OUTPUT_HEADER, "w") as f:
        f.write("\n".join(lines) + "\n")

    total_pu = len(all_pu)
    print(f"\n✓ {OUTPUT_HEADER} generated  (strategy: {strategy})")
    print(f"  NUMA nodes: {nn},  Groups: {ng},  PUs: {total_pu}")
    print(f"\n  {'Group':<8} {'Node':<6} {'PUs':<6}  CPU list")
    print(f"  {'-'*8} {'-'*6} {'-'*6}  {'-'*30}")
    for gi, g in enumerate(groups):
        print(f"  {gi:<8} {g['node']:<6} {len(g['pus']):<6}  {', '.join(str(p) for p in g['pus'])}")
    for ni in range(nn):
        gids = ng_by_node[ni]
        all_np = sorted(set(p for gid in gids for p in groups[gid]["pus"]))
        print(f"  Node {ni}: {len(gids)} groups, {len(all_np)} PUs — {all_np}")

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
def main():
    root = run_lstopo()
    all_objs = collect_objects(root)

    include_ht = "--ht" in sys.argv
    args = [a for a in sys.argv[1:] if a != "--ht"]
    strategy = args[0].lower() if args else None

    if strategy and strategy not in ALL_STRATEGIES:
        print(f"ERROR: unknown strategy '{strategy}'")
        print(f"  Valid: {', '.join(ALL_STRATEGIES)} [--ht]")
        sys.exit(1)

    total_pus = len([o for o in all_objs if o['type'] == 'PU'])
    total_cores = len([o for o in all_objs if o['type'] == 'Core'])
    total_nodes = len([o for o in all_objs if o['type'] == 'NUMANode'])
    print(f"  System — {total_pus} PUs, {total_cores} cores, {total_nodes} NUMA nodes"
          f"  {'(+HT)' if include_ht else '(HT excluded)'}\n")

    if strategy:
        groups = strategy_groups(strategy, all_objs, include_ht)
        if groups is None:
            print(f"  '{strategy}' not available."); sys.exit(1)
        print(f"  Using: {strategy} ({len(groups)} groups)")
        generate(groups, f"{strategy}{'_ht' if include_ht else ''}")
    else:
        best = report_strategies(all_objs, include_ht)
        groups = strategy_groups(best, all_objs, include_ht)
        print(f"\n  Auto-selected: {best}")
        generate(groups, f"{best}{'_ht' if include_ht else ''}")

if __name__ == "__main__":
    main()
