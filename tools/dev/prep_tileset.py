# Prepare clean, tileable pieces from the merged "Sci-Fi Game Tileset" for the engine's
# modular shell. Clusters the 805 loose quads into tiles, picks floor / plain wall /
# cyan-trace wall, recenters each to origin (xy-centered, base at y=0 after +Y-up export),
# and writes each as a GEOMETRY-ONLY glTF under scifi_tileset/<name>/. Per-tile texture
# copies are deleted: the engine loads the one shared atlas (scifi_tileset/textures/) and
# applies it to every tile, so there is no texture duplication.
#   blender.exe --background --python tools/dev/prep_tileset.py
import bpy
import mathutils
import math
import os
import shutil

ROOT = r"C:/Users/rq27/Pulse"
KIT = ROOT + "/assets/external/sketchfab_scifi/scifi_tileset"


def wbbox(o):
    lo = mathutils.Vector((1e30,) * 3); hi = mathutils.Vector((-1e30,) * 3)
    for c in o.bound_box:
        w = o.matrix_world @ mathutils.Vector(c)
        for i in range(3):
            lo[i] = min(lo[i], w[i]); hi[i] = max(hi[i], w[i])
    return lo, hi


def import_loose():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.import_scene.gltf(filepath=KIT + "/scene.gltf")
    ms = [o for o in bpy.context.scene.objects if o.type == "MESH"]
    for o in ms:
        o.select_set(True)
    bpy.context.view_layer.objects.active = ms[0]
    bpy.ops.object.join()
    bpy.ops.object.mode_set(mode="EDIT")
    bpy.ops.mesh.separate(type="LOOSE")
    bpy.ops.object.mode_set(mode="OBJECT")
    return [o for o in bpy.context.scene.objects if o.type == "MESH"]


def cluster(parts, margin=70.0):
    boxes = [wbbox(o) for o in parts]
    parent = list(range(len(parts)))

    def find(a):
        while parent[a] != a:
            parent[a] = parent[parent[a]]; a = parent[a]
        return a

    def ov(b1, b2):
        l1, h1 = b1; l2, h2 = b2
        for i in range(3):
            if h1[i] + margin < l2[i] or h2[i] + margin < l1[i]:
                return False
        return True

    for i in range(len(parts)):
        for j in range(i + 1, len(parts)):
            if ov(boxes[i], boxes[j]):
                parent[find(i)] = find(j)
    groups = {}
    for i in range(len(parts)):
        groups.setdefault(find(i), []).append(i)
    out = []
    for g in groups.values():
        lo = mathutils.Vector((1e30,) * 3); hi = mathutils.Vector((-1e30,) * 3)
        nv = 0
        for idx in g:
            l, h = boxes[idx]
            for k in range(3):
                lo[k] = min(lo[k], l[k]); hi[k] = max(hi[k], h[k])
            nv += len(parts[idx].data.vertices)
        out.append({"idx": set(g), "lo": lo, "hi": hi, "dim": hi - lo, "verts": nv})
    return out


def pick(clusters, want):
    # want: ("wall_plain"|"wall_traced"|"floor")
    def ok_wall(c):
        d = c["dim"]
        foot = sorted([d.x, d.y])
        return 1000 < d.z < 1400 and 700 < foot[1] < 900 and c["lo"].z < 100
    if want == "floor":
        cand = [c for c in clusters if c["dim"].z < 200 and 700 < c["dim"].x < 900
                and 700 < c["dim"].y < 900 and c["lo"].z < 250]
        cand.sort(key=lambda c: c["verts"])           # plainest flat tile
        return cand[0] if cand else None
    if want == "wall_plain":
        cand = [c for c in clusters if ok_wall(c)]
        cand.sort(key=lambda c: (min(c["dim"].x, c["dim"].y), c["verts"]))  # thinnest+plainest
        return cand[0] if cand else None
    if want == "wall_traced":
        # the cyan-trace tech wall: tall, one tile wide, modest thickness (~10), more detail
        cand = [c for c in clusters if ok_wall(c) and 3 < min(c["dim"].x, c["dim"].y) < 120]
        cand.sort(key=lambda c: -c["verts"])          # most detail
        return cand[0] if cand else None
    return None


def export_tile(target_center, name, fill_holes):
    parts = import_loose()
    cl = cluster(parts)
    cl.sort(key=lambda c: ((c["lo"] + c["hi"]) * 0.5 - target_center).length)
    chosen = cl[0]
    bpy.ops.object.select_all(action="DESELECT")
    for i, o in enumerate(parts):
        if i not in chosen["idx"]:
            o.select_set(True)
    bpy.ops.object.delete()
    keep = [o for o in bpy.context.scene.objects if o.type == "MESH"]
    bpy.ops.object.select_all(action="DESELECT")
    for o in keep:
        o.select_set(True)
    bpy.context.view_layer.objects.active = keep[0]
    bpy.ops.object.join()
    obj = bpy.context.view_layer.objects.active
    # Bake the imported parent/world transform into the mesh so obj sits at the root with
    # identity basis - then recenter/rotate operate in world axes (not parent space).
    bpy.ops.object.parent_clear(type="CLEAR_KEEP_TRANSFORM")
    bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)
    if fill_holes:
        bpy.ops.object.mode_set(mode="EDIT")
        bpy.ops.mesh.select_all(action="SELECT")
        bpy.ops.mesh.fill_holes(sides=0)
        bpy.ops.mesh.normals_make_consistent(inside=False)
        bpy.ops.object.mode_set(mode="OBJECT")
    lo, hi = wbbox(obj)
    ctr = (lo + hi) * 0.5
    obj.location.x -= ctr.x; obj.location.y -= ctr.y; obj.location.z -= lo.z
    bpy.context.view_layer.update()
    bpy.ops.object.transform_apply(location=True, rotation=False, scale=False)
    d = hi - lo
    if d.y > d.x:                       # widest horizontal axis -> X (engine wall faces +/-Z)
        obj.rotation_euler.z = math.radians(90)
        bpy.context.view_layer.update()
        bpy.ops.object.transform_apply(location=False, rotation=True, scale=False)
    outdir = KIT + "/" + name
    if os.path.isdir(outdir):
        shutil.rmtree(outdir)
    os.makedirs(outdir)
    bpy.ops.object.select_all(action="DESELECT")
    obj.select_set(True); bpy.context.view_layer.objects.active = obj
    bpy.ops.export_scene.gltf(filepath=outdir + "/scene.gltf", export_format="GLTF_SEPARATE",
                              use_selection=True, export_yup=True, export_apply=True)
    # geometry-only: drop the duplicated atlas copies (engine uses the shared one)
    tex = outdir + "/textures"
    if os.path.isdir(tex):
        shutil.rmtree(tex)
    for f in os.listdir(outdir):
        if f.lower().endswith(".dds"):
            os.remove(outdir + "/" + f)
    lo2, hi2 = wbbox(obj); d2 = hi2 - lo2
    print("TILE %-14s blender(x,y,z)=(%.0f,%.0f,%.0f) engine(w,h,d)=(%.0f,%.0f,%.0f)"
          % (name, d2.x, d2.y, d2.z, d2.x, d2.z, d2.y))


# locate clusters once to get their centers, then export each (re-imports fresh)
parts = import_loose()
cl = cluster(parts)
sel = {}
for w in ("floor", "wall_plain", "wall_traced"):
    c = pick(cl, w)
    if c:
        sel[w] = (c["lo"] + c["hi"]) * 0.5
        print("PICK %-12s dim=(%.0f,%.0f,%.0f) verts=%d"
              % (w, c["dim"].x, c["dim"].y, c["dim"].z, c["verts"]))
    else:
        print("PICK %-12s NONE" % w)

for w, ctr in sel.items():
    export_tile(ctr, w, fill_holes=(w == "wall_traced"))

print("PREP_DONE")
