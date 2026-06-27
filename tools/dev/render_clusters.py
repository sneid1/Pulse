# Render specific tile clusters from the merged Sci-Fi Game Tileset in EEVEE (so the
# cyan emissive traces show), isolated + centered + lit, for visual pick.
#   blender.exe --background --python tools/dev/render_clusters.py
import bpy
import mathutils
import math
import os

ROOT = r"C:/Users/rq27/Pulse"
KIT = ROOT + "/assets/external/sketchfab_scifi/scifi_tileset"
WANT = [18, 2]


def wbbox(o):
    lo = mathutils.Vector((1e30,) * 3); hi = mathutils.Vector((-1e30,) * 3)
    for c in o.bound_box:
        w = o.matrix_world @ mathutils.Vector(c)
        for i in range(3):
            lo[i] = min(lo[i], w[i]); hi[i] = max(hi[i], w[i])
    return lo, hi


def import_parts():
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
        for idx in g:
            l, h = boxes[idx]
            for k in range(3):
                lo[k] = min(lo[k], l[k]); hi[k] = max(hi[k], h[k])
        out.append({"idx": set(g), "lo": lo, "hi": hi, "dim": hi - lo})
    out.sort(key=lambda c: -(c["dim"].x * c["dim"].y * c["dim"].z))
    return out


def boost_emissive():
    for m in bpy.data.materials:
        if not m.use_nodes:
            continue
        for n in m.node_tree.nodes:
            if n.type == "BSDF_PRINCIPLED":
                inp = n.inputs.get("Emission Strength")
                if inp:
                    inp.default_value = max(inp.default_value, 4.0)


for ci in WANT:
    parts = import_parts()
    cl = cluster(parts)
    if ci >= len(cl):
        continue
    keep = cl[ci]["idx"]
    bpy.ops.object.select_all(action="DESELECT")
    for i, o in enumerate(parts):
        if i not in keep:
            o.select_set(True)
    bpy.ops.object.delete()
    boost_emissive()
    ms = [o for o in bpy.context.scene.objects if o.type == "MESH"]
    lo = mathutils.Vector((1e30,) * 3); hi = mathutils.Vector((-1e30,) * 3)
    for o in ms:
        l, h = wbbox(o)
        for i in range(3):
            lo[i] = min(lo[i], l[i]); hi[i] = max(hi[i], h[i])
    ctr = (lo + hi) * 0.5; big = max((hi - lo)) or 100.0
    scn = bpy.context.scene
    scn.render.engine = "BLENDER_EEVEE"
    scn.view_settings.exposure = 1.5
    scn.render.resolution_x = 640; scn.render.resolution_y = 640
    # world ambient
    world = bpy.data.worlds.new("w"); scn.world = world
    world.use_nodes = True
    world.node_tree.nodes["Background"].inputs[0].default_value = (0.05, 0.06, 0.09, 1)
    world.node_tree.nodes["Background"].inputs[1].default_value = 0.6
    # sun
    light = bpy.data.lights.new("sun", "SUN"); light.energy = 4.0
    lo_ = bpy.data.objects.new("sun", light); lo_.rotation_euler = (math.radians(55), 0, math.radians(40))
    bpy.context.collection.objects.link(lo_)
    cam_data = bpy.data.cameras.new("c"); cam_data.type = "ORTHO"
    cam_data.ortho_scale = big * 1.3; cam_data.clip_start = 0.1; cam_data.clip_end = big * 12
    cam = bpy.data.objects.new("c", cam_data)
    cam.location = mathutils.Vector((ctr.x + big, ctr.y - big, ctr.z + big * 0.6))
    cam.rotation_euler = (math.radians(62), 0, math.radians(45))
    bpy.context.collection.objects.link(cam); scn.camera = cam
    scn.render.filepath = ROOT + "/build/cl_%02d.png" % ci
    bpy.ops.render.render(write_still=True)
    d = cl[ci]["dim"]
    print("RENDERED cl_%02d dim=(%.0f,%.0f,%.0f)" % (ci, d.x, d.y, d.z))

print("RENDER_CLUSTERS_DONE")
