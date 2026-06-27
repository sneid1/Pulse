#!/usr/bin/env python3
# Generate config/pulse.rooms from the design handoff "pulse_rooms.json" (the source of
# truth). Also runs the spec's step-9 import validation in Python first, so bad data is
# caught before it ever reaches the engine. Pure transcription: the ASCII grids are copied
# byte-for-byte, each row wrapped in pipes so the loader preserves leading/trailing void.
#
# Usage:  python tools/gen_rooms.py            (reads the default json, writes config/pulse.rooms)
#         python tools/gen_rooms.py --check     (validate only; do not write)
# ASCII only (see CLAUDE.md).

import json
import os
import sys
from collections import deque

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
JSON_PATH = os.path.join(ROOT, "Environment designed", "pulse_rooms.json")
OUT_PATH = os.path.join(ROOT, "config", "pulse.rooms")
KIT_ROOTS = [
    os.path.join(ROOT, "assets", "packs", "pulse_environment", "quaternius", "Modular SciFi MegaKit[Pro]", "glTF"),
    os.path.join(ROOT, "assets", "packs", "pulse_environment", "quaternius", "Sci-Fi Essentials Kit[Pro]", "glTF"),
    os.path.join(ROOT, "assets", "quaternius", "Modular SciFi MegaKit[Pro]", "glTF"),
]

# spec size constraints: (maxCols, maxRows, entries, exits-set)
SIZES = {
    "Small":    (5, 5, 1, {2}),
    "Mid":      (6, 5, 1, {3}),
    "Big":      (8, 6, 1, {3}),
    "Corridor": (7, 3, 1, {1, 2}),
}

# Movement model for reachability (spec step 9):
#   blockers: void(' '), '#', 'o', 'X'
#   passable: '.', 'c', '=', 'p', '/', 'D', 'E', 'H'
#   every '/' must touch an 'H' and have a passable non-H low-side approach opposite it.
#   you may only ENTER an 'H' cell from an 'H' or a '/' (climb via ramp); dropping off H is free.
PASSABLE = set(".c=p/DEH")

DECAL_GROUPS = {
    "Foundry": [
        "Decal_Line_Straight", "Decal_Line_90", "Decal_Caution", "Decal_Code",
        "Decal_Code_2", "Decal_Arrows", "Decal_AccessPoint", "Decal_Dashes",
        "Decal_Authorized", "Decal_Open", "Decal_Sign",
        # bay numbers + curved conduit-shadow lines (broaden the floor signage vocabulary)
        "Decal_0", "Decal_1", "Decal_2", "Decal_3",
        "Decal_Line_Bend1_L", "Decal_Line_Bend1_R", "Decal_Line_90_Round_Large",
    ],
    "Furnace": [
        "Decal_Line_Straight", "Decal_Line_90", "Decal_Caution", "Decal_Warning",
        "Decal_Arrows", "Decal_XSign", "Decal_Dashes",
        "Decal_Line_Bend2_L", "Decal_Line_Bend2_R",
    ],
    "Reliquary": [
        "Decal_Line_Straight", "Decal_Line_90", "Decal_Line_90_Round", "Decal_Logo",
        "Decal_Logo_Small", "Decal_Logo_Letters", "Decal_STRNOV", "Decal_K",
        "Decal_V", "Decal_X", "Decal_Z",
        "Decal_A", "Decal_Line_90_Round_Large",
    ],
}


def kit_assets():
    names = set()
    for kit_root in KIT_ROOTS:
        if not os.path.isdir(kit_root):
            continue
        is_essentials = "Sci-Fi Essentials Kit[Pro]" in kit_root
        for base, _dirs, files in os.walk(kit_root):
            for f in files:
                if f.lower().endswith(".gltf"):
                    stem = os.path.splitext(f)[0]
                    names.add(stem)
                    if is_essentials:
                        names.add("Essentials_" + stem)
    return names


def resolve_asset(name, assets):
    if not name:
        return None
    if name in assets:
        return name
    straight = name + "_Straight"
    if straight in assets:
        return straight
    return None


def split_door_piece(value, biome):
    parts = [p.strip() for p in value.split("+") if p.strip()]
    frames = [p for p in parts if p.startswith("Door_Frame")]
    leaves = [p for p in parts if p.startswith("Door_") and not p.startswith("Door_Frame")]
    if frames:
        frame = frames[0]
    elif biome == "Reliquary":
        frame = "Door_Frame_SquareTall"
    else:
        frame = "Door_Frame_Square"
    leaf = leaves[0] if leaves else (parts[0] if parts else "Door_Metal")
    return frame, leaf


def room_art_fields(data, room):
    biome_name = room["biome"]
    biome = data["biomes"][biome_name]
    frame, leaf = split_door_piece(biome.get("doorPiece", ""), biome_name)
    return {
        "ramp": room.get("rampOverride") or biome.get("ramp", "Platform_Ramp_2"),
        "doorFrame": frame,
        "doorLeaf": leaf,
        "crate": biome.get("dressing", {}).get("cratePiece", ""),
        "dressingPool": biome.get("dressing", {}).get("propPool", []),
        "decalGroup": DECAL_GROUPS.get(biome_name, ["Decal_Line_Straight", "Decal_Caution"]),
    }


def pad(grid):
    w = max(len(r) for r in grid)
    return [r.ljust(w) for r in grid], w


def validate(room):
    name = room["name"]
    size = room["size"]
    errs = []
    raw = room["grid"]
    grid, w = pad(raw)
    h = len(grid)
    def at(i, j):
        if i < 0 or j < 0 or i >= w or j >= h:
            return ' '
        return grid[j][i]

    maxc, maxr, want_e, want_d = SIZES[size]
    if w > maxc or h > maxr:
        errs.append("grid {}x{} exceeds {} max {}x{}".format(w, h, size, maxc, maxr))
    if any(len(r) != len(raw[0]) for r in raw):
        errs.append("rows are not equal width: {}".format([len(r) for r in raw]))

    es = [(i, j) for j in range(h) for i in range(w) if grid[j][i] == 'E']
    ds = [(i, j) for j in range(h) for i in range(w) if grid[j][i] == 'D']
    if len(es) != want_e:
        errs.append("found {} 'E' entries, want {}".format(len(es), want_e))
    if len(ds) not in want_d:
        errs.append("found {} 'D' exits, want one of {}".format(len(ds), sorted(want_d)))

    # Ramp affordance: the high side must touch H, and the low side directly opposite must be
    # an actual walkable approach cell. Reachability alone can miss a ramp backed into a wall.
    dirs = ((0, -1, "N"), (0, 1, "S"), (-1, 0, "W"), (1, 0, "E"))
    for j in range(h):
        for i in range(w):
            if at(i, j) != '/':
                continue
            h_sides = []
            valid = []
            for di, dj, side in dirs:
                if at(i + di, j + dj) != 'H':
                    continue
                h_sides.append(side)
                approach = at(i - di, j - dj)
                if approach in PASSABLE and approach != 'H':
                    valid.append(side)
            if not h_sides:
                errs.append("ramp at ({},{}) has no adjacent H".format(i, j))
            elif not valid:
                samples = []
                for di, dj, side in dirs:
                    if at(i + di, j + dj) == 'H':
                        samples.append("{} approach='{}'".format(side, at(i - di, j - dj)))
                errs.append("ramp at ({},{}) has blocked low-side approach ({})".format(i, j, ", ".join(samples)))

    # reachability BFS from the (first) entry over the directed passable graph
    if es:
        start = es[0]
        seen = {start}
        q = deque([start])
        while q:
            i, j = q.popleft()
            for di, dj in ((0, -1), (0, 1), (-1, 0), (1, 0)):
                ni, nj = i + di, j + dj
                if ni < 0 or nj < 0 or ni >= w or nj >= h:
                    continue
                c = grid[nj][ni]
                if c not in PASSABLE or (ni, nj) in seen:
                    continue
                # cannot climb onto an H except from a ramp or another H cell
                if c == 'H' and grid[j][i] not in 'H/':
                    continue
                seen.add((ni, nj))
                q.append((ni, nj))
        unreached = [(i, j) for j in range(h) for i in range(w)
                     if grid[j][i] in PASSABLE and (i, j) not in seen]
        if unreached:
            errs.append("{} passable cell(s) unreachable from E: {}".format(len(unreached), unreached[:6]))
    return errs


def validate_refs(data, room, assets):
    fields = room_art_fields(data, room)
    refs = []
    for key in ("family", "floor", "top", "cover", "pillar", "focal", "daisFloor"):
        if room.get(key):
            refs.append((key, room[key]))
    for key in ("ramp", "doorFrame", "doorLeaf", "crate"):
        if fields.get(key):
            refs.append((key, fields[key]))
    for n in fields["dressingPool"]:
        refs.append(("dressingPool", n))
    for n in fields["decalGroup"]:
        refs.append(("decalGroup", n))

    errs = []
    for label, name in refs:
        if not resolve_asset(name, assets):
            errs.append("kit ref {}='{}' not found".format(label, name))
    return errs


def emit_room(data, room):
    art = room_art_fields(data, room)
    lines = []
    lines.append("NAME: {}".format(room["name"]))
    lines.append("SIZE: {}".format(room["size"]))
    lines.append("BIOME: {}".format(room["biome"]))
    lines.append("FAMILY: {}".format(room["family"]))
    lines.append("FLOOR: {}".format(room["floor"]))
    lines.append("RAMP: {}".format(art["ramp"]))
    lines.append("DOOR_FRAME: {}".format(art["doorFrame"]))
    lines.append("DOOR_LEAF: {}".format(art["doorLeaf"]))
    lines.append("CRATE: {}".format(art["crate"]))
    lines.append("DRESSING_POOL: {}".format(",".join(art["dressingPool"])))
    lines.append("DECAL_GROUP: {}".format(",".join(art["decalGroup"])))
    if room.get("top"):
        lines.append("TOP: {}".format(room["top"]))
    if room.get("cover"):
        lines.append("COVER: {}".format(room["cover"]))
    if room.get("pillar"):
        lines.append("PILLAR: {}".format(room["pillar"]))
    if room.get("focal"):
        lines.append("FOCAL: {}".format(room["focal"]))
    if room.get("daisFloor"):
        lines.append("DAIS: {}".format(room["daisFloor"]))
    lines.append("GRID:")
    for r in room["grid"]:
        lines.append("|{}|".format(r))
    return "\n".join(lines)


HEADER = """\
// Pulse - hand-crafted room templates (data-driven; no recompile - just relaunch).
// GENERATED by tools/gen_rooms.py from "Environment designed/pulse_rooms.json" (the design
// handoff source of truth). Edit the JSON + regenerate; do not hand-edit this file.
//
// Format: NAME:/SIZE:/BIOME:/FAMILY:/FLOOR:/RAMP:/DOOR_FRAME:/DOOR_LEAF:/CRATE:/
// DRESSING_POOL:/DECAL_GROUP:/TOP:/COVER:/PILLAR:/FOCAL: header lines, then
// GRID: + pipe-delimited rows (|<cells>|, 1 char = one 4 m cell, space = void). The pipes
// preserve leading/trailing void against editors that strip trailing whitespace.
// Legend:  (space)=void  .=floor  #=solid block  o=pillar  c=crate  ==low wall  H=raised
//   platform  /=ramp  X=focal prop  p=dressing prop  D=exit door  E=entrance+spawn.
// Sizes:  Small <=5x5 (1E+2D)  Mid <=6x5 (1E+3D)  Big <=8x6 (1E+3D)  Corridor <=7x3 (1E+1-2D).
// 30 rooms: 10 per biome (Foundry / Furnace / Reliquary), across all four sizes.
"""


def main():
    check_only = "--check" in sys.argv
    with open(JSON_PATH, "r", encoding="utf-8") as f:
        data = json.load(f)
    rooms = data["rooms"]
    assets = kit_assets()

    ok = True
    by_bs = {}
    for room in rooms:
        errs = validate(room)
        errs += validate_refs(data, room, assets)
        by_bs.setdefault((room["biome"], room["size"]), 0)
        by_bs[(room["biome"], room["size"])] += 1
        tag = "PASS" if not errs else "FAIL"
        if errs:
            ok = False
        print("[{}] {:8} {:9} {}".format(tag, room["id"], room["size"], room["name"]))
        for e in errs:
            print("        - {}".format(e))

    print("\nroom coverage (biome, size) -> count:")
    for biome in ("Foundry", "Furnace", "Reliquary"):
        row = {sz: by_bs.get((biome, sz), 0) for sz in ("Corridor", "Small", "Mid", "Big")}
        print("  {:10} {}".format(biome, row))

    if not ok:
        print("\nVALIDATION FAILED - not writing config.")
        return 1

    if check_only:
        print("\nvalidation OK ({} rooms, {} kit assets indexed, zero unresolved kit refs).".format(len(rooms), len(assets)))
        return 0

    body = HEADER + "\n" + "\n\n".join(emit_room(data, r) for r in rooms) + "\n"
    # enforce ASCII-only (CLAUDE.md hard rule)
    body.encode("ascii")
    with open(OUT_PATH, "w", encoding="ascii", newline="\n") as f:
        f.write(body)
    print("\nwrote {} ({} rooms, validation OK).".format(OUT_PATH, len(rooms)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
