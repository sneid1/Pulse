#!/usr/bin/env python3
# Import a glb/gltf, report mesh/armature/bbox, and render a front view PNG so we can SEE the shape.
#   blender -b --python tools/blender/inspect_render.py -- <model> <out.png>
import bpy, sys, math
from mathutils import Vector

argv = sys.argv[sys.argv.index("--") + 1:]
src, out = argv[0], argv[1]
bpy.ops.wm.read_homefile(use_empty=True)
low = src.lower()
if low.endswith(".fbx"):
    bpy.ops.import_scene.fbx(filepath=src)
else:
    bpy.ops.import_scene.gltf(filepath=src)

meshes = [o for o in bpy.data.objects if o.type == 'MESH']
arms   = [o for o in bpy.data.objects if o.type == 'ARMATURE']
mn = Vector((1e9, 1e9, 1e9)); mx = Vector((-1e9, -1e9, -1e9))
tris = 0
for o in meshes:
    tris += sum(len(p.vertices) - 2 for p in o.data.polygons)
    for c in o.bound_box:
        w = o.matrix_world @ Vector(c)
        for i in range(3):
            mn[i] = min(mn[i], w[i]); mx[i] = max(mx[i], w[i])
dim = mx - mn; center = (mn + mx) * 0.5
print("MESHES=%d ARMATURES=%d TRIS=%d" % (len(meshes), len(arms), tris))
print("BBOX dim x=%.2f y=%.2f z=%.2f (z=up)" % (dim.x, dim.y, dim.z))
print("names: " + ", ".join(o.name for o in meshes[:12]))

r = max(dim.x, dim.y, dim.z, 0.1)
cam_d = bpy.data.cameras.new("C"); cam = bpy.data.objects.new("C", cam_d)
bpy.context.scene.collection.objects.link(cam)
cam.location = center + Vector((0.0, -r * 2.3, 0.0)); cam.rotation_euler = (math.radians(90), 0, 0)
cam_d.ortho_scale = r * 1.7; cam_d.type = 'ORTHO'
bpy.context.scene.camera = cam
l = bpy.data.lights.new("L", 'SUN'); lo = bpy.data.objects.new("L", l)
bpy.context.scene.collection.objects.link(lo); lo.rotation_euler = (math.radians(55), 0, math.radians(35)); l.energy = 3.0
sc = bpy.context.scene
sc.render.engine = 'BLENDER_WORKBENCH'
sc.render.resolution_x = 640; sc.render.resolution_y = 800
sc.render.filepath = out
bpy.ops.render.render(write_still=True)
print("RENDERED " + out)
