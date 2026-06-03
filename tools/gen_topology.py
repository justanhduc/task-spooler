#!/usr/bin/env python3
"""
gen_topology.py — Generate topology headers via hwloc-calc (zero XML parsing).

All topology queries follow the natural top-down hierarchy:
    NUMANode / L3 / L2 / L1 → Core → PU

Precomputed once at startup:
  - core_pu_map:    Core:N → PU list  (top-down)
  - core_node_map:  derived from NUMANode:N → Core (top-down), then inverted

Strategy groups query type:i → Core (top-down), then expand cores via cache.

Output: topology_{strategy}.h for EVERY valid strategy. Pick one and copy/link to
        topology.h before building.

Usage:
    python3 gen_topology.py                 # generate all non-trivial strategies
    python3 gen_topology.py by_l2           # generate only the specified strategy
    python3 gen_topology.py by_l2 --ht      # include HT siblings
    python3 gen_topology.py by_l2 --no-ht   # no HT at all (core == PU)
"""

import subprocess, sys

MAX_GROUPS = 256

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


def hwloc_intersect(obj_spec, target_type, extra = ""):
    """Return list of ints: obj_spec --intersect target_type."""
    out = hwloc([obj_spec, "--intersect", target_type, extra])
    if not out:
        return []
    return [int(x) for x in out.split(",")]


# ── Topology cache (precomputed once, all top-down) ─────────────────────────

def build_topology_cache(no_ht=False):
    """Precompute core→PU and core→NUMA maps.

    All queries follow the natural hierarchy direction:
      - Core:N → PU      (top-down: core contains PUs)
      - NUMANode:N → Core (top-down: NUMA contains cores, then inverted)

    When no_ht is True, core == PU (no HT), skip all per-core queries.
    """
    total_cores = hwloc_count("Core")
    total_nodes = hwloc_count("NUMANode")
    print(f"  Precomputing topology for {total_cores} cores, "
          f"{total_nodes} NUMA nodes ...", end=" ", flush=True)

    # ── core→PU  (top-down: Core:N → PU) ──
    core_pu_map = {}
    if no_ht:
        for i in range(total_cores):
            core_pu_map[i] = [i]
    else:
        for i in range(total_cores):
            core_pu_map[i] = sorted(hwloc_intersect(f"Core:{i}", "PU", "--physical"))

    # ── node→cores (top-down: NUMANode:N → Core), then invert to core→node ──
    core_node_map = {}
    for ni in range(total_nodes):
        for c in hwloc_intersect(f"NUMANode:{ni}", "Core"):
            core_node_map[c] = ni
    print("core_node_map:", core_node_map)
    # Fill any core not covered (shouldn't happen, but safety)
    for i in range(total_cores):
        if i not in core_node_map:
            core_node_map[i] = -1

    print("done.")
    return core_pu_map, core_node_map


# ── PU collection (from cache) ──────────────────────────────────────────────

def collect_pus(core_indices, core_pu_map, include_ht):
    """Given core OS indices, return sorted PU list.

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
        return -1
    return core_node_map.get(core_indices[0], -1)


def groups_equal(g1, g2):
    """Return True if two groups lists produce identical topology."""
    if len(g1) != len(g2):
        return False
    for a, b in zip(g1, g2):
        if a["node"] != b["node"] or a["pus"] != b["pus"]:
            return False
    return True


# ── Group building ──────────────────────────────────────────────────────────

def build_groups(strategy, core_pu_map, core_node_map, include_ht,
                 total_cores, total_pus):
    """Return list of {node, pus} dicts, or None if strategy is trivial.

    A strategy is trivial (returns None) when its object count equals
    total_cores, or equals total_pus when --ht is active.  This means every
    group would contain exactly one core/PU — useless for grouping.
    by_core is always kept (never skipped as trivial).
    """
    hwloc_type = STRATEGIES[strategy]
    count = hwloc_count(hwloc_type)
    if count == 0:
        return None

    # Skip trivial: each object maps 1:1 to a core or PU (except by_core)
    if strategy != "by_core":
        if count == total_cores:
            print(f"  {strategy}: count={count} == cores={total_cores}, skipping (trivial)")
            return None
        if include_ht and count == total_pus:
            print(f"  {strategy}: count={count} == PUs={total_pus}, skipping (trivial)")
            return None

    trivial = (count == total_cores)  # each object maps 1:1 to a core
    groups = []
    for i in range(count):
        cores = [i] if trivial else hwloc_intersect(f"{hwloc_type}:{i}", "Core")
        pus = collect_pus(cores, core_pu_map, include_ht)
        if not pus:
            continue

        node = i if strategy == "by_numa" else get_node(cores, core_node_map)
        groups.append({"node": node, "pus": pus})

    return groups or None


# ── topology.h generation ───────────────────────────────────────────────────

def generate(groups, strategy, suffix=""):
    """Write topology_{strategy}{suffix}.h ."""
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

    # MAX_GROUPS_PER_NODE: max groups on any single node
    max_gpn = max(len(gids) for gids in ng_by_node.values()) if ng_by_node else ng

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
        f"/* Strategy: {strategy}{suffix} */",
        "",
        "#ifndef TOPOLOGY_H",
        "#define TOPOLOGY_H",
        "",
        "#define NUM_NODES            " + str(nn),
        "#define NUM_GROUPS           " + str(ng),
        "#define MAX_CORES_PER_GROUP  " + str(max_cpg),
        "#define MAX_OS_CPU           " + str(max_os),
        "#define MAX_GROUPS_PER_NODE  " + str(max_gpn),
        "",
    ] + macro + [
        "",
        "#endif /* TOPOLOGY_H */",
        "",
    ]

    fname = f"topology_{strategy}{suffix}.h"
    with open(fname, "w") as f:
        f.write("\n".join(lines) + "\n")

    total_pu = len(all_pu)
    print(f"\n  -> {fname}  "
          f"(nodes={nn}, groups={ng}, max_groups_per_node={max_gpn}, PUs={total_pu})")
    print(f"  {'Group':<8} {'Node':<6} {'PUs':<6}  CPU list")
    print(f"  {'-'*8} {'-'*6} {'-'*6}  {'-'*30}")
    for gi, g in enumerate(groups):
        print(f"  {gi:<8} {g['node']:<6} {len(g['pus']):<6}  "
              f"{', '.join(str(p) for p in g['pus'])}")
    for ni in range(nn):
        gids = ng_by_node[ni]
        all_np = sorted(set(p for gid in gids for p in groups[gid]["pus"]))
        print(f"  Node {ni}: {len(gids)} groups, {len(all_np)} PUs — {all_np}")


# ── Auto-select best strategy ───────────────────────────────────────────────

def select_best(results):
    """Given list of (name, groups, ng, min_pu, max_pu, total_pu) or
       (name, None) for skipped, pick best."""
    TARGET_AVG = 4
    best, best_groups, best_dist = None, None, None
    for r in results:
        if r[1] is None:
            continue
        s, data, ng, _mp, _xp, tp = r
        if ng <= 1:
            continue
        dist = abs(tp / ng - TARGET_AVG)
        if best is None or dist < best_dist:
            best, best_groups, best_dist = s, data, dist

    # Fallback: any valid strategy
    if best is None:
        for r in results:
            if r[1] is not None:
                best, best_groups = r[0], r[1]
                break

    return best, best_groups


# ── Main ────────────────────────────────────────────────────────────────────

def main():
    no_ht = "--no-ht" in sys.argv
    include_ht = not no_ht and "--ht" in sys.argv
    args = [a for a in sys.argv[1:] if a not in ("--ht", "--no-ht")]
    requested = args[0].lower() if args else None

    if requested and requested not in STRATEGIES:
        print(f"ERROR: unknown strategy '{requested}'")
        print(f"  Valid: {', '.join(STRATEGIES)} [--ht] [--no-ht]")
        sys.exit(1)

    total_pus = hwloc_count("PU") if not no_ht else hwloc_count("Core")
    total_cores = hwloc_count("Core")
    total_nodes = hwloc_count("NUMANode")
    ht_label = " (no HT)" if no_ht else (" (+HT)" if include_ht else " (HT excluded)")
    print(f"  System — {total_pus} PUs, {total_cores} cores, {total_nodes} NUMA nodes{ht_label}")

    # Precompute once — all strategies share this cache
    core_pu_map, core_node_map = build_topology_cache(no_ht)
    print()

    suffix = "_ht" if include_ht else ""
    strategies = [requested] if requested else list(STRATEGIES)

    # Build all requested strategies, skipping trivial ones
    results = []
    for s in strategies:
        groups = build_groups(s, core_pu_map, core_node_map, include_ht,
                              total_cores, total_pus)
        if groups is None:
            results.append((s, None))
            continue
        ng = len(groups)
        mp = min(len(g["pus"]) for g in groups)
        xp = max(len(g["pus"]) for g in groups)
        tp = sum(len(g["pus"]) for g in groups)
        results.append((s, groups, ng, mp, xp, tp))

    # Detect duplicate group configurations — keep first, skip later ones
    dup_of = {}  # strategy → first strategy with identical groups
    valid = [r for r in results if r[1] is not None]
    for i in range(len(valid)):
        si = valid[i][0]
        for j in range(i + 1, len(valid)):
            sj = valid[j][0]
            if sj in dup_of:
                continue
            if groups_equal(valid[i][1], valid[j][1]):
                dup_of[sj] = si

    # Report
    best, best_groups = select_best(results)
    print(f"  {'Strategy':<14} {'Groups':<8} {'PUs/group':<12} {'Note'}")
    print(f"  {'-'*14} {'-'*8} {'-'*12} {'-'*40}")
    for r in results:
        s = r[0]
        if r[1] is None:
            note = " (skipped — trivial)"
            print(f"  {s:<14} {'—':<8} {'—':<12} {note}")
            continue
        if s in dup_of:
            note = f" (skipped — duplicate of {dup_of[s]})"
            print(f"  {s:<14} {'—':<8} {'—':<12} {note}")
            continue
        ng, mp, xp = r[2], r[3], r[4]
        note = (" <- best" if s == best
                else " (too coarse)" if ng <= 1
                else "")
        n2 = f"{mp}" if mp == xp else f"{mp}–{xp}"
        print(f"  {s:<14} {ng:<8} {n2:<12} {note}")
    print()

    # Generate .h files (skip trivial and duplicates)
    generated = []
    for r in results:
        s, data = r[0], r[1]
        if data is None or s in dup_of:
            continue
        generate(data, s, suffix)
        generated.append(s)

    if not generated:
        print("ERROR: no non-trivial strategy available.")
        sys.exit(1)

    # Fallback: if no best was picked (e.g. all ng <= 1), use first generated
    if best is None:
        best = generated[0]

    # Show best recommendation
    if requested is None:
        print(f"\n  Best: {best} — copy it to topology.h:")
    print(f"  cp topology_{best}{suffix}.h topology.h")

    # Summary of all generated files
    if requested is None:
        print(f"\n  All generated ({len(generated)} files):")
        for s in generated:
            print(f"    topology_{s}{suffix}.h")


if __name__ == "__main__":
    main()
