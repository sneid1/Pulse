#!/usr/bin/env python3
# Audit Pulse arena asset coverage. The report is a source/config reference audit:
# if a mesh name or external kit package is reachable from runtime room data or the
# arena/game source, it counts as used. This keeps the tool deterministic and cheap.

import argparse
import json
import os
import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]

MEGAKIT_ROOT = ROOT / "assets" / "packs" / "pulse_environment" / "quaternius" / "Modular SciFi MegaKit[Pro]" / "glTF"
ESSENTIALS_ROOT = ROOT / "assets" / "packs" / "pulse_environment" / "quaternius" / "Sci-Fi Essentials Kit[Pro]" / "glTF"
SKETCHFAB_ROOT = ROOT / "assets" / "external" / "sketchfab_scifi"
POLYHAVEN_ROOT = ROOT / "assets" / "external" / "polyhaven"

REFERENCE_FILES = [
    ROOT / "src" / "Game" / "Wasteland.cpp",
    ROOT / "src" / "Game" / "Wasteland.hpp",
    ROOT / "src" / "Game" / "PulseGame.cpp",
    ROOT / "src" / "main.cpp",
    ROOT / "config" / "pulse.rooms",
    ROOT / "Environment designed" / "pulse_rooms.json",
]


def slash(path):
    return str(path).replace("\\", "/")


def rel(path):
    try:
        return slash(path.relative_to(ROOT))
    except ValueError:
        return slash(path)


def read_reference_text():
    chunks = []
    for path in REFERENCE_FILES:
        if not path.exists():
            continue
        chunks.append(path.read_text(encoding="utf-8", errors="ignore"))
    return "\n".join(chunks)


def collect_gltf_assets(root):
    assets = []
    if not root.exists():
        return assets
    for path in sorted(root.rglob("*.gltf")):
        assets.append({
            "name": path.stem,
            "rel": rel(path),
            "category": path.parent.name,
        })
    return assets


def collect_sketchfab_packages(root):
    packages = []
    if not root.exists():
        return packages
    for path in sorted(root.iterdir()):
        if not path.is_dir():
            continue
        gltf_files = sorted(p for p in path.rglob("*") if p.suffix.lower() in (".gltf", ".glb"))
        if not gltf_files:
            continue
        packages.append({
            "name": path.name,
            "rel": rel(path),
            "files": [rel(p) for p in gltf_files],
        })
    return packages


def collect_polyhaven_sets(root):
    if not root.exists():
        return []
    return sorted(p.name for p in root.iterdir() if p.is_dir())


def token_used(text, name):
    pattern = r"(?<![A-Za-z0-9_])" + re.escape(name) + r"(?![A-Za-z0-9_])"
    return re.search(pattern, text) is not None


def path_or_name_used(text, package):
    if token_used(text, package["name"]):
        return True
    for file_path in package["files"]:
        if file_path in text or file_path.replace("/", "\\") in text:
            return True
    return False


def summarize_named_assets(label, assets, text, ambiguous_names=None):
    ambiguous_names = set(ambiguous_names or [])
    by_name = {}
    duplicate_names = set()
    for asset in assets:
        if asset["name"] in by_name:
            duplicate_names.add(asset["name"])
        by_name.setdefault(asset["name"], []).append(asset)

    def used_name(name, entries):
        if name not in ambiguous_names:
            return token_used(text, name)
        return any(entry["rel"] in text or entry["rel"].replace("/", "\\") in text for entry in entries)

    used_names = sorted(name for name, entries in by_name.items() if used_name(name, entries))
    used_assets = []
    for name in used_names:
        used_assets.extend(by_name[name])

    by_category = {}
    for asset in assets:
        cat = asset["category"]
        bucket = by_category.setdefault(cat, {"present": 0, "used": 0})
        bucket["present"] += 1
        if asset["name"] in used_names:
            bucket["used"] += 1

    return {
        "label": label,
        "present": len(assets),
        "used": len(used_assets),
        "used_names": used_names,
        "unused_names": sorted(name for name in by_name if name not in used_names),
        "duplicates": sorted(duplicate_names),
        "by_category": by_category,
    }


def summarize_sketchfab(packages, text):
    used = sorted(pkg["name"] for pkg in packages if path_or_name_used(text, pkg))
    return {
        "label": "Sketchfab sci-fi",
        "present": len(packages),
        "used": len(used),
        "used_names": used,
        "unused_names": sorted(pkg["name"] for pkg in packages if pkg["name"] not in used),
    }


def summarize_polyhaven(sets, text):
    used = sorted(name for name in sets if token_used(text, name))
    return {
        "label": "Polyhaven PBR",
        "present": len(sets),
        "used": len(used),
        "used_names": used,
        "unused_names": sorted(name for name in sets if name not in used),
    }


def build_snapshot():
    text = read_reference_text()
    megakit_assets = collect_gltf_assets(MEGAKIT_ROOT)
    essentials_assets = collect_gltf_assets(ESSENTIALS_ROOT)
    megakit_names = {asset["name"] for asset in megakit_assets}
    essentials_names = {asset["name"] for asset in essentials_assets}
    duplicate_quaternius_names = megakit_names.intersection(essentials_names)
    return {
        "megakit": summarize_named_assets("Quaternius MegaKit", megakit_assets, text),
        "essentials": summarize_named_assets("Quaternius Sci-Fi Essentials", essentials_assets, text, duplicate_quaternius_names),
        "sketchfab": summarize_sketchfab(collect_sketchfab_packages(SKETCHFAB_ROOT), text),
        "polyhaven": summarize_polyhaven(collect_polyhaven_sets(POLYHAVEN_ROOT), text),
    }


def table_line(cols):
    return "| " + " | ".join(cols) + " |"


def render_report(snapshot, baseline=None):
    lines = []
    lines.append("# Pulse Environment Asset Coverage")
    lines.append("")
    lines.append("Reference scope: runtime arena/game source plus generated room config and room JSON.")
    lines.append("")
    lines.append(table_line(["Kit", "Present", "Used", "Unused", "Use %", "Delta vs baseline"]))
    lines.append(table_line(["---", "---:", "---:", "---:", "---:", "---:"]))
    for key in ("megakit", "essentials", "sketchfab", "polyhaven"):
        cur = snapshot[key]
        prev_used = None if baseline is None or key not in baseline else baseline[key]["used"]
        delta = "" if prev_used is None else "{:+d}".format(cur["used"] - prev_used)
        present = cur["present"]
        used = cur["used"]
        percent = 0.0 if present == 0 else used * 100.0 / present
        lines.append(table_line([
            cur["label"],
            str(present),
            str(used),
            str(max(0, present - used)),
            "{:.1f}".format(percent),
            delta,
        ]))

    lines.append("")
    if baseline:
        lines.append("Baseline used counts: " + ", ".join(
            "{} {}".format(baseline[key]["label"], baseline[key]["used"])
            for key in ("megakit", "essentials", "sketchfab", "polyhaven")
            if key in baseline
        ))
        lines.append("")

    for key in ("megakit", "essentials"):
        cur = snapshot[key]
        lines.append("## {}".format(cur["label"]))
        lines.append("")
        if cur["by_category"]:
            lines.append(table_line(["Category", "Present", "Used"]))
            lines.append(table_line(["---", "---:", "---:"]))
            for cat in sorted(cur["by_category"]):
                data = cur["by_category"][cat]
                lines.append(table_line([cat, str(data["present"]), str(data["used"])]))
            lines.append("")
        if cur["duplicates"]:
            lines.append("Duplicate stems: {}".format(", ".join(cur["duplicates"])))
            lines.append("")
        lines.append("Used examples: {}".format(", ".join(cur["used_names"][:40]) if cur["used_names"] else "none"))
        lines.append("")
        lines.append("Unused examples: {}".format(", ".join(cur["unused_names"][:40]) if cur["unused_names"] else "none"))
        lines.append("")

    for key in ("sketchfab", "polyhaven"):
        cur = snapshot[key]
        lines.append("## {}".format(cur["label"]))
        lines.append("")
        lines.append("Used: {}".format(", ".join(cur["used_names"]) if cur["used_names"] else "none"))
        lines.append("")
        lines.append("Unused: {}".format(", ".join(cur["unused_names"]) if cur["unused_names"] else "none"))
        lines.append("")

    return "\n".join(lines) + "\n"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", default=str(ROOT / "tools" / "reports" / "environment_asset_coverage.md"))
    parser.add_argument("--baseline")
    parser.add_argument("--write-baseline")
    args = parser.parse_args()

    snapshot = build_snapshot()
    baseline = None
    if args.baseline:
        with open(args.baseline, "r", encoding="ascii") as f:
            baseline = json.load(f)

    out_path = Path(args.out)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    report = render_report(snapshot, baseline)
    report.encode("ascii")
    out_path.write_text(report, encoding="ascii", newline="\n")

    if args.write_baseline:
        baseline_path = Path(args.write_baseline)
        baseline_path.parent.mkdir(parents=True, exist_ok=True)
        baseline_path.write_text(json.dumps(snapshot, indent=2, sort_keys=True) + "\n",
                                 encoding="ascii", newline="\n")

    for key in ("megakit", "essentials", "sketchfab", "polyhaven"):
        cur = snapshot[key]
        print("{}: {}/{} used".format(cur["label"], cur["used"], cur["present"]))
    print("wrote {}".format(rel(out_path)))
    if args.write_baseline:
        print("wrote baseline {}".format(rel(Path(args.write_baseline))))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
