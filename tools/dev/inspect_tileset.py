# Headless inspect of a Sketchfab tileset: import the gltf, report the world-space
# bbox, and render TOP / FRONT / PERSP textured previews so the tile layout is visible.
#   blender.exe --background --python tools/dev/inspect_tileset.py
import bpy
import mathutils

KIT = r"C:/Users/rq27/Pulse/assets/external/sketchfab_scifi/scifi_tileset/scene.gltf"
OUT = r"C:/Users/rq27/Pulse/build/tileset_"

bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.gltf(filepath=KIT)

meshes = [o for o in bpy.context.scene.objects if o.type == "MESH"]
print("MESH_OBJECTS", len(meshes))

# World-space bbox over all mesh corners.
lo = mathutils.Vector((1e30, 1e30, 1e30))
hi = mathutils.Vector((-1e30, -1e30, -1e30))
for o in meshes:
    for c in o.bound_box:
        w = o.matrix_world @ mathutils.Vector(c)
        for i in range(3):
            lo[i] = min(lo[i], w[i])
            hi[i] = max(hi[i], w[i])
dim = hi - lo
ctr = (hi + lo) * 0.5
print("WORLD_BBOX lo=(%.3f,%.3f,%.3f) hi=(%.3f,%.3f,%.3f)" % (lo.x, lo.y, lo.z, hi.x, hi.y, hi.z))
print("WORLD_DIMS dx=%.3f dy=%.3f dz=%.3f" % (dim.x, dim.y, dim.z))

# Separate everything by loose parts and report each cluster bbox (sorted by size desc),
# so distinct tiles become visible as separate objects.
for o in meshes:
    o.select_set(True)
bpy.context.view_layer.objects.active = meshes[0]
bpy.ops.object.join()
joined = bpy.context.view_layer.objects.active
bpy.ops.object.mode_set(mode="EDIT")
bpy.ops.mesh.separate(type="LOOSE")
bpy.ops.object.mode_set(mode="OBJECT")
parts = [o for o in bpy.context.scene.objects if o.type == "MESH"]
print("LOOSE_PARTS", len(parts))


def bbox(o):
    p_lo = mathutils.Vector((1e30, 1e30, 1e30))
    p_hi = mathutils.Vector((-1e30, -1e30, -1e30))
    for c in o.bound_box:
        w = o.matrix_world @ mathutils.Vector(c)
        for i in range(3):
            p_lo[i] = min(p_lo[i], w[i])
            p_hi[i] = max(p_hi[i], w[i])
    return p_lo, p_hi


info = []
for o in parts:
    pl, ph = bbox(o)
    d = ph - pl
    info.append((d.x * d.y * d.z, o.name, pl, ph, d, len(o.data.vertices)))
info.sort(reverse=True)
print("--- top 30 parts by bbox volume ---")
for vol, nm, pl, ph, d, nv in info[:30]:
    print("  %-22s dims=(%.2f,%.2f,%.2f) ctr=(%.2f,%.2f,%.2f) verts=%d"
          % (nm[:22], d.x, d.y, d.z, (pl.x + ph.x) / 2, (pl.y + ph.y) / 2, (pl.z + ph.z) / 2, nv))

# --- render textured previews ---
scn = bpy.context.scene
scn.render.engine = "BLENDER_WORKBENCH"
scn.display.shading.color_type = "TEXTURE"
scn.display.shading.light = "STUDIO"
scn.render.resolution_x = 1100
scn.render.resolution_y = 800
scn.render.film_transparent = False

big = max(dim.x, dim.y, dim.z)


def add_cam(name, loc, rot, ortho_scale):
    cam_data = bpy.data.cameras.new(name)
    cam_data.type = "ORTHO"
    cam_data.ortho_scale = ortho_scale
    cam_data.clip_start = 1.0
    cam_data.clip_end = big * 6.0
    cam = bpy.data.objects.new(name, cam_data)
    cam.location = loc
    cam.rotation_euler = rot
    bpy.context.collection.objects.link(cam)
    return cam


import math
views = {
    "top":   (mathutils.Vector((ctr.x, ctr.y, ctr.z + big)), (0, 0, 0), max(dim.x, dim.y) * 1.15),
    "front": (mathutils.Vector((ctr.x, ctr.y - big, ctr.z)), (math.radians(90), 0, 0), max(dim.x, dim.z) * 1.15),
    "persp": (mathutils.Vector((ctr.x + big * 0.8, ctr.y - big * 0.8, ctr.z + big * 0.7)),
              (math.radians(58), 0, math.radians(45)), big * 1.25),
}
for nm, (loc, rot, osc) in views.items():
    cam = add_cam("cam_" + nm, loc, rot, osc)
    scn.camera = cam
    scn.render.filepath = OUT + nm + ".png"
    bpy.ops.render.render(write_still=True)
    print("RENDERED", scn.render.filepath)

print("INSPECT_DONE")
