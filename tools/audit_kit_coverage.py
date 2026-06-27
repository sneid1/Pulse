#!/usr/bin/env python3
# Kit coverage audit for the Pulse arena environment (Workstream A).
#
# Reports, per art kit, which meshes are PRESENT on disk vs. which the runtime
# arena assembler can actually INSTANTIATE. The "used" set is computed by
# replaying, in Python, the exact resolution the C++ does at load time:
#   * scanQuatAssets()  - the glTF filename registry (MegaKit + Essentials),
#   * loadQuaternius()  - the hard-coded floor/wall/top/column/door/crate/dress
#                         pools + the per-biome fill pools + the shared extra
#                         pre-load list,
#   * the per-template pre-load expansion (preWallFamily / preTopFamily corners,
#     preBoth "_Straight", and the header/dressing/decal refs from every room in
#     config/pulse.rooms),
#   * resolveQuatAssetName() (exact key, then stem, then "<name>_Straight").
# It also scans Wasteland.cpp for the external (Sketchfab / PolyHaven) kit
# references so the whole "downloaded but unused" picture is in one place.
#
# This is a static analysis (no build/run needed); it mirrors the assembler's
# resolver so the numbers match what the game can place. Run before and after a
# density pass to see the utilisation delta.
#
# Usage:  python tools/audit_kit_coverage.py            (print report to stdout)
#         python tools/audit_kit_coverage.py --md FILE   (also write a markdown report)
# ASCII only (see CLAUDE.md).

import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

MEGAKIT = os.path.join(ROOT, "assets", "packs", "pulse_environment", "quaternius",
                       "Modular SciFi MegaKit[Pro]", "glTF")
ESSENTIALS = os.path.join(ROOT, "assets", "packs", "pulse_environment", "quaternius",
                          "Sci-Fi Essentials Kit[Pro]", "glTF")
ROOMS = os.path.join(ROOT, "config", "pulse.rooms")
WASTELAND_CPP = os.path.join(ROOT, "src", "Game", "Wasteland.cpp")

# ----------------------------------------------------------------------------
# 1. Registry: replicate scanQuatAssets(). MegaKit root first (bare names win),
#    then Essentials (bare names if free + "Essentials_" alias). Maps key -> rel.
# ----------------------------------------------------------------------------

def build_registry():
    reg = {}            # key -> (kit, relpath)
    present = {}         # kit -> { relpath: category }
    roots = [("MegaKit", MEGAKIT, ""), ("Essentials", ESSENTIALS, "Essentials_")]
    for kit, root, alias in roots:
        present[kit] = {}
        if not os.path.isdir(root):
            continue
        for base, _dirs, files in os.walk(root):
            for f in files:
                if not f.lower().endswith(".gltf"):
                    continue
                stem = os.path.splitext(f)[0]
                rel = os.path.relpath(os.path.join(base, f), root).replace("\\", "/")
                cat = rel.split("/")[0] if "/" in rel else "(root)"
                present[kit][rel] = cat
                if stem not in reg:
                    reg[stem] = (kit, rel)
                if alias:
                    a = alias + stem
                    if a not in reg:
                        reg[a] = (kit, rel)
    return reg, present


def resolve(name, reg):
    """resolveQuatAssetName: exact, then stem, then name+'_Straight'."""
    if not name:
        return None
    if name in reg:
        return name
    key = name
    if "/" in key or "\\" in key or os.path.splitext(key)[1]:
        key = os.path.splitext(os.path.basename(key))[0]
    if key in reg:
        return key
    s = key + "_Straight"
    if s in reg:
        return s
    return None


# ----------------------------------------------------------------------------
# 2. Used set: the hard pools + the shared pre-load list + every room template's
#    expanded refs. Mirrors loadQuaternius() / the pre-load loop in Wasteland.cpp.
# ----------------------------------------------------------------------------

HARD_POOL = [
    # floors / walls / top / column / doors / crates / dress (loadQuaternius 910-934)
    "Platform_Simple", "Platform_DarkPlates", "Platform_Squares", "Platform_Metal",
    "WallPadded_Straight", "WallAstra_Straight", "WallBand_Straight", "WallPipe_Straight",
    "TopPlates_Straight", "Column_Round", "Door_Frame_Square", "Door_Metal",
    "Door_DarkMetal", "Door_Simple",
    "Prop_Crate1", "Prop_Crate2", "Prop_Crate3", "Prop_Barrel_Large",
    "Prop_Computer", "Prop_AccessPoint", "Prop_Cable_2", "Prop_Pipe_Medium_Straight",
    "Platform_Ramp_2", "Platform_Ramp_4Wide", "Platform_Stairs_2",
    # per-biome fill pools (937-957)
    "Prop_Cable_1", "Prop_Cable_3", "Prop_Pipe_Thick_Straight", "Prop_Light_Wide", "Prop_Fan_Small",
    "Prop_Pipe_Thick_Curve", "Prop_Vent_Big", "Prop_Vent_Wide", "Prop_Vent_Small",
    "Prop_Barrel_Small", "Prop_WallBand_BrokenPlate", "Prop_Clamp",
    "Prop_Light_Floor", "Prop_Light_Corner", "Prop_Chest", "Prop_ItemHolder",
]

# Shared extra pre-load (Wasteland.cpp 1077-1096).
SHARED_EXTRA = [
    "Prop_Rail_4", "Prop_Rail_3", "Prop_Rail_2", "Prop_Rail_Incline_Long_L", "Prop_Rail_Incline_Long_R",
    "Platform_Rails_2", "Platform_Rails_4", "Platform_Rails_4Wide", "Platform_Rails_4WideTall",
    "Prop_Light_Floor", "Prop_Light_Wide", "Prop_AccessPoint", "Prop_PipeHolder",
    "Prop_Vent_Wide", "Prop_Vent_Big", "Prop_Vent_Small", "Prop_Cable_1", "Prop_Cable_2", "Prop_Cable_3", "Prop_Cable_4",
    "Prop_Pipe_Thick_Straight", "Prop_Pipe_Medium_Straight", "Prop_Pipe_Small_Straight",
    "Prop_Light_Small", "Prop_Fan_Small", "Prop_Barrel_Small", "Prop_Clamp", "Prop_Chest", "Prop_ItemHolder", "Prop_Computer",
    "Platform_CenterPlate", "Platform_CenterPlate_Curve", "Platform_Round1", "Platform_Round2",
    "Platform_RedAccent", "Platform_X", "Platform_Metal", "Platform_Metal_Curve",
    "Platform_Metal2", "Platform_Squares", "Platform_Simple_Curve",
    "TopCables_Straight", "TopCables_Straight_Hanging",
    "BottomAccent_Straight", "BottomMetal_Straight", "BottomSimple_Straight",
    "BottomAccent_Corner_Square_Inner", "BottomAccent_Corner_Square_Outer_1", "BottomAccent_Corner_Square_Outer_2",
    "BottomMetal_Corner_Square_Inner", "BottomMetal_Corner_Square_Outer_1", "BottomMetal_Corner_Square_Outer_2",
    "BottomSimple_Corner_Square_Inner", "BottomSimple_Corner_Square_Outer_1", "BottomSimple_Corner_Square_Outer_2",
    "Decal_Caution", "Decal_Warning", "Decal_Arrows", "Decal_Code", "Decal_Code_2",
    "Decal_Line_Straight", "Decal_Line_90", "Decal_Line_90_Round", "Decal_Logo", "Decal_Logo_Small",
    "Decal_Logo_Letters", "Decal_STRNOV", "Decal_AccessPoint", "Decal_K", "Decal_V", "Decal_X", "Decal_Z",
    "Decal_Dashes", "Decal_Authorized", "Decal_Open", "Decal_Sign", "Decal_XSign",
]

WALL_CORNERS = ["_Corner_Square_Inner", "_Corner_Square_Outer",
                "_Corner_Round_Inner", "_Corner_Round_Outer"]
TOP_CORNERS = WALL_CORNERS + ["_Corner_Curve_Inner", "_Corner_Curve_Outer",
                              "_Curve_Round_Inner", "_Curve_Round_Outer"]


def family_stem(n):
    for suff in ("_Straight_Broken", "_Straight"):
        i = n.find(suff)
        if i != -1:
            n = n[:i]
    return n


def parse_rooms(path):
    rooms = []
    cur = None
    if not os.path.exists(path):
        return rooms
    with open(path, "r", encoding="ascii") as f:
        for line in f:
            line = line.rstrip("\n")
            t = line.strip()
            if t.startswith("//") or not t:
                continue
            if ":" not in t:
                continue
            key, _, val = t.partition(":")
            key = key.strip().upper()
            val = val.strip()
            if key == "NAME":
                cur = {"dress": [], "decal": []}
                rooms.append(cur)
            if cur is None:
                continue
            if key == "FAMILY": cur["family"] = val
            elif key == "FLOOR": cur["floor"] = val
            elif key == "TOP": cur["top"] = val
            elif key == "COVER": cur["cover"] = val
            elif key == "PILLAR": cur["pillar"] = val
            elif key == "FOCAL": cur["focal"] = val
            elif key == "DAIS": cur["dais"] = val
            elif key == "RAMP": cur["ramp"] = val
            elif key == "DOOR_FRAME": cur["doorFrame"] = val
            elif key == "DOOR_LEAF": cur["doorLeaf"] = val
            elif key == "CRATE": cur["crate"] = val
            elif key == "DRESSING_POOL": cur["dress"] = [x.strip() for x in val.split(",") if x.strip()]
            elif key == "DECAL_GROUP": cur["decal"] = [x.strip() for x in val.split(",") if x.strip()]
    return rooms


def referenced_names(rooms):
    """Every name the pre-load loop feeds to quatPieceByName."""
    names = set(HARD_POOL) | set(SHARED_EXTRA)
    for r in rooms:
        fam = r.get("family", "")
        if fam:
            names.add(fam); names.add(fam + "_Straight")
            for s in WALL_CORNERS:
                names.add(family_stem(fam) + s)
        top = r.get("top", "")
        if top:
            names.add(top); names.add(top + "_Straight")
            for s in TOP_CORNERS:
                names.add(family_stem(top) + s)
        cover = r.get("cover", "")
        if cover:
            names.add(cover); names.add(cover + "_Straight")
        for k in ("floor", "pillar", "focal", "dais", "ramp", "doorFrame", "doorLeaf", "crate"):
            if r.get(k):
                names.add(r[k])
        for n in r.get("dress", []):
            names.add(n)
        for n in r.get("decal", []):
            names.add(n)
    return names


# ----------------------------------------------------------------------------
# 3. External kit references (Sketchfab / PolyHaven) scraped from Wasteland.cpp.
# ----------------------------------------------------------------------------

def external_refs():
    sketchfab, polyhaven = set(), set()
    if os.path.exists(WASTELAND_CPP):
        txt = open(WASTELAND_CPP, "r", encoding="ascii", errors="replace").read()
        for m in re.finditer(r'assets/external/sketchfab_scifi/([A-Za-z0-9_]+)/', txt):
            sketchfab.add(m.group(1))
        for m in re.finditer(r'loadPbr\("([A-Za-z0-9_]+)"', txt):
            polyhaven.add(m.group(1))
    return sketchfab, polyhaven


def list_dirs(parent):
    if not os.path.isdir(parent):
        return []
    return sorted(d for d in os.listdir(parent) if os.path.isdir(os.path.join(parent, d)))


# ----------------------------------------------------------------------------
# 4. Meshy bespoke prop pack. loadMeshyEnvironmentProps() loads EVERY .glb under
#    each role folder it visits, into role-matched, tier-aware pools. So "used"
#    = every glb under a visited role; placement is by role (focals->'X',
#    anchors->corner pockets, wall_details->edge pockets, floor_details->lanes,
#    common/*->architectural joints), deterministic per seed.
# ----------------------------------------------------------------------------

MESHY = os.path.join(ROOT, "assets", "packs", "pulse_environment", "meshy")
MESHY_BIOME_ROLES = ("focals", "anchors", "floor_details", "wall_details")
MESHY_COMMON_ROLES = ("base_trim", "ceiling_duct", "ceiling_spine", "deck_support",
                      "door_lintel", "door_side", "door_threshold", "floor_hatch",
                      "stair_finisher", "wall_alcove", "wall_seam")


def meshy_inventory():
    present = []     # (bucket, role, name)
    if os.path.isdir(MESHY):
        for base, _dirs, files in os.walk(MESHY):
            for f in files:
                if not f.lower().endswith(".glb"):
                    continue
                rel = os.path.relpath(os.path.join(base, f), MESHY).replace("\\", "/")
                parts = rel.split("/")
                bucket = parts[0] if len(parts) > 1 else "(root)"
                role = parts[1] if len(parts) > 2 else (parts[0] if len(parts) == 2 else "(root)")
                present.append((bucket, role))
    return present


# ----------------------------------------------------------------------------

def main():
    reg, present = build_registry()
    rooms = parse_rooms(ROOMS)
    refs = referenced_names(rooms)

    # Resolve every referenced name to a concrete (kit, rel) file.
    used_files = {"MegaKit": set(), "Essentials": set()}
    unresolved = sorted(n for n in refs if resolve(n, reg) is None)
    for n in refs:
        key = resolve(n, reg)
        if key is None:
            continue
        kit, rel = reg[key]
        used_files[kit].add(rel)

    out = []
    def p(s=""):
        out.append(s)

    p("PULSE arena kit coverage audit")
    p("(static replay of the assembler resolver; matches what the game can instantiate)")
    p("rooms parsed from config/pulse.rooms: {}".format(len(rooms)))
    p("")

    # Per-kit, per-category present vs used.
    grand = {}
    for kit in ("MegaKit", "Essentials"):
        cats = {}
        for rel, cat in present[kit].items():
            cats.setdefault(cat, {"present": [], "used": []})
            cats[cat]["present"].append(rel)
            if rel in used_files[kit]:
                cats[cat]["used"].append(rel)
        tot_p = sum(len(v["present"]) for v in cats.values())
        tot_u = sum(len(v["used"]) for v in cats.values())
        grand[kit] = (tot_u, tot_p)
        p("=" * 64)
        pct = (100.0 * tot_u / tot_p) if tot_p else 0.0
        p("{} : {}/{} meshes used ({:.0f}%)".format(kit, tot_u, tot_p, pct))
        p("-" * 64)
        for cat in sorted(cats):
            v = cats[cat]
            p("  {:14} {:3}/{:<3} used".format(cat, len(v["used"]), len(v["present"])))
        p("")
        for cat in sorted(cats):
            v = cats[cat]
            unused = sorted(os.path.splitext(os.path.basename(x))[0] for x in v["present"] if x not in v["used"])
            if unused:
                p("  UNUSED {} ({}):".format(cat, len(unused)))
                line = "    "
                for name in unused:
                    if len(line) + len(name) + 2 > 100:
                        p(line); line = "    "
                    line += name + ", "
                p(line.rstrip(", "))
        p("")

    p("=" * 64)
    p("TOTAL Quaternius: {}/{} ({}/{} MegaKit, {}/{} Essentials)".format(
        grand["MegaKit"][0] + grand["Essentials"][0],
        grand["MegaKit"][1] + grand["Essentials"][1],
        grand["MegaKit"][0], grand["MegaKit"][1],
        grand["Essentials"][0], grand["Essentials"][1]))
    if unresolved:
        p("")
        p("REFERENCED BUT UNRESOLVED ({}): {}".format(len(unresolved), ", ".join(unresolved)))

    # External kits.
    sf_used, ph_used = external_refs()
    sf_present = list_dirs(os.path.join(ROOT, "assets", "external", "sketchfab_scifi"))
    ph_present = list_dirs(os.path.join(ROOT, "assets", "external", "polyhaven"))
    p("")
    p("=" * 64)
    p("EXTERNAL kits (referenced in Wasteland.cpp)")
    p("-" * 64)
    p("  Sketchfab: {}/{} used".format(len(sf_used & set(sf_present)), len(sf_present)))
    p("    used  : {}".format(", ".join(sorted(sf_used)) or "(none)"))
    p("    unused: {}".format(", ".join(sorted(set(sf_present) - sf_used)) or "(none)"))
    p("  PolyHaven: {}/{} used".format(len(ph_used & set(ph_present)), len(ph_present)))
    p("    used  : {}".format(", ".join(sorted(ph_used)) or "(none)"))
    p("    unused: {}".format(", ".join(sorted(set(ph_present) - ph_used)) or "(none)"))

    # Meshy bespoke pack.
    meshy = meshy_inventory()
    if meshy:
        from collections import Counter
        per_bucket = Counter(b for b, _r in meshy)
        visited = 0
        for b, r in meshy:
            if b in ("foundry", "furnace", "reliquary", "shared") and r in MESHY_BIOME_ROLES:
                visited += 1
            elif b == "common" and r in MESHY_COMMON_ROLES:
                visited += 1
        p("")
        p("=" * 64)
        p("MESHY bespoke prop pack (assets/packs/pulse_environment/meshy)")
        p("-" * 64)
        p("  {}/{} glb loaded into role-matched pools (loadPool loads every glb per role dir)".format(visited, len(meshy)))
        for b in sorted(per_bucket):
            p("    {:12} {}".format(b, per_bucket[b]))
        p("  placement: role + tier matched, edge-weighted, deterministic per seed (not random)")

    report = "\n".join(out)
    print(report)

    if "--md" in sys.argv:
        i = sys.argv.index("--md")
        if i + 1 < len(sys.argv):
            with open(sys.argv[i + 1], "w", encoding="ascii", newline="\n") as f:
                f.write("```\n" + report + "\n```\n")
            print("\nwrote " + sys.argv[i + 1])
    return 0


if __name__ == "__main__":
    sys.exit(main())
