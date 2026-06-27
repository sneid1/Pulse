# Dump the Quaternius MegaKit inventory (name + WxHxD + minY) per category, for the asset catalog.
import json, os, glob
MK = "assets/quaternius/Modular SciFi MegaKit[Pro]/glTF"


def dims(p):
    g = json.load(open(p)); mn = [1e30] * 3; mx = [-1e30] * 3
    for m in g.get("meshes", []):
        for prim in m["primitives"]:
            a = g["accessors"][prim["attributes"]["POSITION"]]
            for i in range(3): mn[i] = min(mn[i], a["min"][i]); mx[i] = max(mx[i], a["max"][i])
    return [round(mx[i] - mn[i], 2) for i in range(3)], round(mn[1], 2)


for cat in ["Walls", "Platforms", "Columns", "Props", "Decals"]:
    rows = []
    for p in sorted(glob.glob(glob.escape(f"{MK}/{cat}") + "/*.gltf")):
        d, miny = dims(p); rows.append((os.path.basename(p)[:-5], d, miny))
    print(f"\n===== {cat} ({len(rows)}) =====")
    for nm, d, my in rows:
        print(f"{nm}\t{d[0]}x{d[1]}x{d[2]}\tminY={my}")
