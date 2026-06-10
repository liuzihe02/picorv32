#!/usr/bin/env python3
"""consolidate_coverage.py — merge per-run coverage JSON files into one cumulative report.

Usage:
    python consolidate_coverage.py [--dir <testbench_dir>] [--out merged_coverage.json]

Reads all coverage_*.json files in the target directory, sums bin counts across runs,
recomputes coverpoint / covergroup / total coverage percentages, and writes the merged
result as JSON.  Also writes a corresponding merged XML via pyvsc.
"""

import argparse
import glob
import json
import os
import sys


def load_files(directory):
    pattern = os.path.join(directory, "coverage_*.json")
    paths = sorted(glob.glob(pattern))
    if not paths:
        print(f"No coverage_*.json files found in {directory}", file=sys.stderr)
        return []
    datasets = []
    sys.path.insert(0, directory)
    for p in paths:
        try:
            with open(p) as fh:
                datasets.append(json.load(fh))
        except Exception as exc:
            print(f"Warning: skipping {p}: {exc}", file=sys.stderr)
    print(f"Loaded {len(datasets)} coverage files")
    return datasets


def merge_bins(bins_list):
    """Sum counts across multiple samples of the same bin; keep goal & recompute hit."""
    merged = {}
    for sample in bins_list:
        for b in sample:
            key = b["name"]
            if key not in merged:
                merged[key] = {"name": key, "count": 0, "goal": b["goal"]}
            merged[key]["count"] += b["count"]
    result = []
    for b in merged.values():
        b["hit"] = b["count"] >= b["goal"]
        result.append(b)
    return result


def merge_crosses(crosses_list):
    if not crosses_list:
        return []
    merged = {}
    for sample_list in crosses_list:
        for cr in sample_list:
            key = cr["name"]
            if key not in merged:
                merged[key] = {"name": key, "coverage": 0.0, "weight": cr.get("weight", 1),
                               "bins_total": cr["bins_total"], "bins_hit": 0}
            # crosses don't have per-bin counts exposed, preserve max bins_hit
            merged[key]["bins_hit"] = max(merged[key]["bins_hit"], cr.get("bins_hit", 0))
    result = []
    for cr in merged.values():
        if cr["bins_total"] > 0:
            cr["coverage"] = (cr["bins_hit"] / cr["bins_total"]) * 100.0 * cr["weight"]
        result.append(cr)
    return result


def cp_coverage(bins):
    if not bins:
        return 0.0
    hit = sum(1 for b in bins if b["hit"])
    return (hit / len(bins)) * 100.0


def item_coverage(items, key):
    if not items:
        return 0.0
    total_weight = sum(it.get("weight", 1) for it in items)
    if total_weight == 0:
        return 0.0
    weighted = sum(it.get("weight", 1) * it.get(key, 0.0) for it in items)
    return weighted / total_weight


def consolidate(datasets):
    if not datasets:
        return None

    # Build index: cg_name -> cp_name -> [bins list from each dataset]
    cg_map = {}  # cg_name -> {cp_name: [bins_list, ...], crosses: [crosses_list, ...]}
    for ds in datasets:
        for cg in ds.get("covergroups", []):
            cg_name = cg["name"]
            if cg_name not in cg_map:
                cg_map[cg_name] = {
                    "instname": cg.get("instname", cg_name),
                    "weight": cg.get("weight", 1),
                    "coverpoints": {},
                    "crosses": [],
                }
            for cp in cg.get("coverpoints", []):
                cp_name = cp["name"]
                if cp_name not in cg_map[cg_name]["coverpoints"]:
                    cg_map[cg_name]["coverpoints"][cp_name] = {
                        "weight": cp.get("weight", 1),
                        "bins_total": cp["bins_total"],
                        "bins_samples": [],
                    }
                cg_map[cg_name]["coverpoints"][cp_name]["bins_samples"].append(cp.get("bins", []))
            cg_map[cg_name]["crosses"].append(cg.get("crosses", []))

    # Build merged covergroups
    covergroups = []
    for cg_name in sorted(cg_map):
        info = cg_map[cg_name]
        coverpoints = []
        for cp_name in sorted(info["coverpoints"]):
            cp_info = info["coverpoints"][cp_name]
            merged_bins = merge_bins(cp_info["bins_samples"])
            cov_pct = cp_coverage(merged_bins)
            coverpoints.append({
                "name": cp_name,
                "coverage": cov_pct,
                "weight": cp_info["weight"],
                "bins_total": len(merged_bins),
                "bins_hit": sum(1 for b in merged_bins if b["hit"]),
                "bins": merged_bins,
            })
        merged_crosses = merge_crosses(info["crosses"])
        cg_pct = item_coverage(coverpoints + merged_crosses, "coverage")
        covergroups.append({
            "name": cg_name,
            "instname": info["instname"],
            "coverage": cg_pct,
            "weight": info["weight"],
            "coverpoints": coverpoints,
            "crosses": merged_crosses,
        })

    total = item_coverage(covergroups, "coverage")
    return {"total_coverage": total, "covergroups": covergroups}


def write_merged_json(report, path):
    with open(path, "w") as fh:
        json.dump(report, fh, indent=2)
    print(f"Merged JSON written to {path}")


def write_merged_xml(report, xml_path):
    """Write a human-readable XML summary from the merged report."""
    lines = ['<?xml version="1.0" encoding="UTF-8"?>']
    lines.append('<coverage_report>')
    lines.append(f'  <total_coverage>{report["total_coverage"]:.4f}</total_coverage>')
    for cg in report["covergroups"]:
        lines.append(f'  <covergroup name="{cg["name"]}" instname="{cg.get("instname", cg["name"])}"'
                     f' coverage="{cg["coverage"]:.4f}" weight="{cg.get("weight", 1)}">')
        for cp in cg["coverpoints"]:
            lines.append(f'    <coverpoint name="{cp["name"]}" coverage="{cp["coverage"]:.4f}"'
                         f' bins_hit="{cp["bins_hit"]}" bins_total="{cp["bins_total"]}"'
                         f' weight="{cp.get("weight", 1)}">')
            for b in cp["bins"]:
                lines.append(f'      <bin name="{b["name"]}" count="{b["count"]}"'
                             f' goal="{b["goal"]}" hit="{str(b["hit"]).lower()}"/>')
            lines.append('    </coverpoint>')
        for cr in cg.get("crosses", []):
            lines.append(f'    <cross name="{cr["name"]}" coverage="{cr["coverage"]:.4f}"'
                         f' bins_hit="{cr.get("bins_hit", 0)}" bins_total="{cr.get("bins_total", 0)}"/>')
        lines.append('  </covergroup>')
    lines.append('</coverage_report>')
    with open(xml_path, "w") as fh:
        fh.write("\n".join(lines) + "\n")
    print(f"Merged XML written to {xml_path}")


def main():
    parser = argparse.ArgumentParser(description="Consolidate coverage_*.json files")
    parser.add_argument("--dir", default=os.path.dirname(os.path.abspath(__file__)),
                        help="Directory containing coverage_*.json files")
    parser.add_argument("--out", default="consolidated_coverage.json",
                        help="Output JSON path (default: consolidated_coverage.json)")
    parser.add_argument("--xml", default=None,
                        help="Output XML path (default: <out_prefix>.xml)")
    args = parser.parse_args()

    datasets = load_files(args.dir)
    if not datasets:
        sys.exit(1)

    merged = consolidate(datasets)
    if merged is None:
        sys.exit(1)

    json_out = args.out
    if not os.path.isabs(json_out):
        json_out = os.path.join(args.dir, json_out)
    write_merged_json(merged, json_out)

    xml_out = args.xml
    if xml_out is None:
        xml_out = os.path.splitext(json_out)[0] + ".xml"
    if not os.path.isabs(xml_out):
        xml_out = os.path.join(args.dir, xml_out)
    write_merged_xml(merged, xml_out)

    print(f"\n=== Consolidated Coverage ===")
    print(f"Total: {merged['total_coverage']:.1f}%")
    for cg in merged["covergroups"]:
        print(f"  {cg['name']}: {cg['coverage']:.1f}%")
        for cp in cg["coverpoints"]:
            hit = sum(1 for b in cp["bins"] if b["hit"])
            print(f"    {cp['name']}: {cp['coverage']:.1f}%  ({hit}/{cp['bins_total']} bins hit)")


if __name__ == "__main__":
    main()
