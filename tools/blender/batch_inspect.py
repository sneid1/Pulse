#!/usr/bin/env python3
# Render a front view + report mesh/tris/bbox for each model passed.
#   blender -b --python tools/blender/batch_inspect.py -- <outdir> <model1> <model2> ...
import bpy, sys, math, os
from mathutils import Vector

argv = sys.argv[sys.argv.index("--") + 1:]
outdir = argv[0]; items = argv[1:]
os.makedirs(outdir, exist_ok=True)

for src in items:
    name = os.path.splitext(os.path.basename(src))[0]
    bpy.ops.wm.read_homefile(use_empty=True)
    low = src.lower()
    if low.endswith(".obj"):
        bpy.ops.wm.obj_import(filepath=src)
    elif low.endswith(".fbx"):
        bpy.ops.import_scene.fbx(filepath=src)
    else:
        bpy.ops.import_scene.gltf(filepath=src)
    meshes = [o for o in bpy.data.objects if o.type == 'MESH']
    mn = Vector((1e9, 1e9, 1e9)); mx = Vector((-1e9, -1e9, -1e9)); tris = 0
    for o in meshes:
        tris += sum(len(p.vertices) - 2 for p in o.data.polygons)
        for c in o.bound_box:
            w = o.matrix_world @ Vector(c)
            for i in range(3):
                mn[i] = min(mn[i], w[i]); mx[i] = max(mx[i], w[i])
    dim = mx - mn; center = (mn + mx) * 0.5
    print("ENEMY %s MESHES=%d TRIS=%d DIM x=%.2f y=%.2f z=%.2f" % (name, len(meshes), tris, dim.x, dim.y, dim.z))
    r = max(dim.x, dim.y, dim.z, 0.1)
    cam_d = bpy.data.cameras.new("C"); cam = bpy.data.objects.new("C", cam_d)
    bpy.context.scene.collection.objects.link(cam)
    cam.location = center + Vector((0.0, -r * 2.3, 0.0)); cam.rotation_euler = (math.radians(90), 0, 0)
    cam_d.type = 'ORTHO'; cam_d.ortho_scale = r * 1.75
    bpy.context.scene.camera = cam
    l = bpy.data.lights.new("L", 'SUN'); lo = bpy.data.objects.new("L", l)
    bpy.context.scene.collection.objects.link(lo); lo.rotation_euler = (math.radians(55), 0, math.radians(35)); l.energy = 3.0
    sc = bpy.context.scene
    sc.render.engine = 'BLENDER_WORKBENCH'; sc.render.resolution_x = 440; sc.render.resolution_y = 580
    sc.render.filepath = os.path.join(outdir, name + ".png")
    bpy.ops.render.render(write_still=True)
print("DONE")
