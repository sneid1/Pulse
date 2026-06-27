# Extract one clean WALL tile and one clean FLOOR tile from the merged "Sci-Fi Game
# Tileset" (805 loose quads laid out in scene space). Cluster loose parts into tiles by
# spatial proximity, classify by shape, recenter the best wall + floor to origin, and
# export each as a standalone glTF (separate .gltf + .bin + textures) that the engine's
# KitProp loader handles. Renders each pick for verification.
#   blender.exe --background --python tools/dev/extract_tiles.py
import bpy
import mathutils
import math
import os

ROOT = r"C:/Users/rq27/Pulse"
KIT = ROOT + "/assets/external/sketchfab_scifi/scifi_tileset"


def reset_import():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.import_scene.gltf(filepath=KIT + "/scene.gltf")
    meshes = [o for o in bpy.context.scene.objects if o.type == "MESH"]
    for o in meshes:
        o.select_set(True)
    bpy.context.view_layer.objects.active = meshes[0]
    bpy.ops.object.join()
    j = bpy.context.view_layer.objects.active
    bpy.ops.object.mode_set(mode="EDIT")
    bpy.ops.mesh.separate(type="LOOSE")
    bpy.ops.object.mode_set(mode="OBJECT")
    return [o for o in bpy.context.scene.objects if o.type == "MESH"]


def wbbox(o):
    lo = mathutils.Vector((1e30, 1e30, 1e30))
    hi = mathutils.Vector((-1e30, -1e30, -1e30))
    for c in o.bound_box:
        w = o.matrix_world @ mathutils.Vector(c)
        for i in range(3):
            lo[i] = min(lo[i], w[i]); hi[i] = max(hi[i], w[i])
    return lo, hi


def cluster(parts, margin=70.0):
    # union-find on bbox-overlap (expanded by margin)
    boxes = [wbbox(o) for o in parts]
    parent = list(range(len(parts)))

    def find(a):
        while parent[a] != a:
            parent[a] = parent[parent[a]]; a = parent[a]
        return a

    def overlap(b1, b2):
        l1, h1 = b1; l2, h2 = b2
        for i in range(3):
            if h1[i] + margin < l2[i] or h2[i] + margin < l1[i]:
                return False
        return True

    for i in range(len(parts)):
        for j in range(i + 1, len(parts)):
            if overlap(boxes[i], boxes[j]):
                parent[find(i)] = find(j)
    groups = {}
    for i in range(len(parts)):
        groups.setdefault(find(i), []).append(i)
    out = []
    for g in groups.values():
        lo = mathutils.Vector((1e30, 1e30, 1e30)); hi = mathutils.Vector((-1e30, -1e30, -1e30))
        nv = 0
        for idx in g:
            l, h = boxes[idx]
            for k in range(3):
                lo[k] = min(lo[k], l[k]); hi[k] = max(hi[k], h[k])
            nv += len(parts[idx].data.vertices)
        out.append({"idx": g, "lo": lo, "hi": hi, "dim": hi - lo, "verts": nv})
    return out


parts = reset_import()
clusters = cluster(parts)
clusters.sort(key=lambda c: -(c["dim"].x * c["dim"].y * c["dim"].z))
print("CLUSTERS", len(clusters))
for i, c in enumerate(clusters[:20]):
    d = c["dim"]; lo = c["lo"]
    print("  #%-2d parts=%-3d verts=%-5d dim=(%.0f,%.0f,%.0f) minz=%.0f"
          % (i, len(c["idx"]), c["verts"], d.x, d.y, d.z, lo.z))


def score_wall(c):
    # Prefer a PLAIN full-height wall quad (no door/console hole): tall, one-tile wide,
    # as thin and low-vert as possible (the kit's flat blank wall plane, cluster #18).
    d = c["dim"]
    h = d.z
    foot = sorted([d.x, d.y])  # [thin, wide]
    if h < 1000 or h > 1400:
        return -1
    if foot[1] < 700 or foot[1] > 900:   # one tile wide
        return -1
    if c["lo"].z > 100:                   # must start near floor
        return -1
    return 100000 - foot[0] * 50 - c["verts"]  # thinnest + plainest wins


def score_floor(c):
    d = c["dim"]
    if d.z > 250:
        return -1
    foot = sorted([d.x, d.y])
    if foot[0] < 500 or foot[1] > 1100:
        return -1
    if c["lo"].z > 300:
        return -1
    return foot[0] * foot[1] - abs(d.x - d.y) * 100  # prefer big + squareish


def best(scorer):
    ranked = sorted(((scorer(c), c) for c in clusters), key=lambda t: -t[0])
    return ranked[0] if ranked and ranked[0][0] > 0 else (None, None)


sw, wall = best(score_wall)
sf, floor = best(score_floor)
print("WALL score=%s dim=%s" % (sw, tuple(round(v) for v in wall["dim"]) if wall else None))
print("FLOOR score=%s dim=%s" % (sf, tuple(round(v) for v in floor["dim"]) if floor else None))


def export_cluster(c, dest, rotate_width_to_x):
    # isolate, recenter (xy center -> origin, min z -> 0), optional 90deg yaw so the wider
    # horizontal axis runs along X, then export a standalone glTF.
    reset = reset_import()  # fresh copy so transforms are clean
    cl = cluster(reset)
    # match the chosen cluster by nearest bbox center
    tgt = (c["lo"] + c["hi"]) * 0.5
    cl.sort(key=lambda k: ((k["lo"] + k["hi"]) * 0.5 - tgt).length)
    chosen = cl[0]
    keep = set(chosen["idx"])
    bpy.ops.object.select_all(action="DESELECT")
    for i, o in enumerate(reset):
        if i in keep:
            o.select_set(True)
    bpy.context.view_layer.objects.active = reset[next(iter(keep))]
    bpy.ops.object.join()
    obj = bpy.context.view_layer.objects.active
    lo, hi = wbbox(obj)
    ctr = (lo + hi) * 0.5
    obj.location.x -= ctr.x
    obj.location.y -= ctr.y
    obj.location.z -= lo.z
    bpy.context.view_layer.update()
    d = hi - lo
    if rotate_width_to_x and d.y > d.x:
        obj.rotation_euler.z = math.radians(90)
    bpy.context.view_layer.update()
    bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)
    outdir = KIT + "/" + dest
    os.makedirs(outdir, exist_ok=True)
    bpy.ops.object.select_all(action="DESELECT")
    obj.select_set(True)
    bpy.context.view_layer.objects.active = obj
    bpy.ops.export_scene.gltf(filepath=outdir + "/scene.gltf", export_format="GLTF_SEPARATE",
                              use_selection=True, export_yup=True, export_apply=True)
    lo2, hi2 = wbbox(obj)
    d2 = hi2 - lo2
    print("EXPORTED %s blender_dims=(%.0f,%.0f,%.0f) -> engine(x,y,z)=(%.0f,%.0f,%.0f)"
          % (dest, d2.x, d2.y, d2.z, d2.x, d2.z, d2.y))
    return obj


if wall:
    export_cluster(wall, "wall_tile", rotate_width_to_x=True)
if floor:
    export_cluster(floor, "floor_tile", rotate_width_to_x=False)


def render(dest):
    path = KIT + "/" + dest + "/scene.gltf"
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.import_scene.gltf(filepath=path)
    ms = [o for o in bpy.context.scene.objects if o.type == "MESH"]
    lo = mathutils.Vector((1e30,) * 3); hi = mathutils.Vector((-1e30,) * 3)
    for o in ms:
        l, h = wbbox(o)
        for i in range(3):
            lo[i] = min(lo[i], l[i]); hi[i] = max(hi[i], h[i])
    ctr = (lo + hi) * 0.5; big = max((hi - lo))
    scn = bpy.context.scene
    scn.render.engine = "BLENDER_WORKBENCH"
    scn.display.shading.color_type = "TEXTURE"
    scn.render.resolution_x = 700; scn.render.resolution_y = 700
    cam_data = bpy.data.cameras.new("c"); cam_data.type = "ORTHO"
    cam_data.ortho_scale = big * 1.4; cam_data.clip_start = 0.1; cam_data.clip_end = big * 10
    cam = bpy.data.objects.new("c", cam_data)
    cam.location = mathutils.Vector((ctr.x + big, ctr.y - big, ctr.z + big * 0.8))
    cam.rotation_euler = (math.radians(58), 0, math.radians(45))
    bpy.context.collection.objects.link(cam); scn.camera = cam
    scn.render.filepath = ROOT + "/build/extract_" + dest + ".png"
    bpy.ops.render.render(write_still=True)
    print("RENDERED", scn.render.filepath)


render("wall_tile")
render("floor_tile")
print("EXTRACT_DONE")
