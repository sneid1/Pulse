#!/usr/bin/env python3
# Batch-convert Meshy character meshes (obj/glb/fbx) -> Mixamo-ready FBX: join to one mesh, decimate
# to a tri budget, export Y-up / -Z-forward FBX.
#   blender -b --python tools/blender/to_mixamo_fbx.py -- <outdir> <targetTris> <src1> [src2 ...]
import bpy, sys, os

argv = sys.argv[sys.argv.index("--") + 1:]
outdir = argv[0]; target = int(argv[1]); srcs = argv[2:]
os.makedirs(outdir, exist_ok=True)

for src in srcs:
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
    if not meshes:
        print("SKIP %s (no mesh)" % name); continue
    def tc():
        return sum(sum(len(p.vertices) - 2 for p in o.data.polygons) for o in meshes)
    before = tc()
    if len(meshes) > 1:
        bpy.ops.object.select_all(action='DESELECT')
        for o in meshes:
            o.select_set(True)
        bpy.context.view_layer.objects.active = meshes[0]
        bpy.ops.object.join()
        meshes = [bpy.context.view_layer.objects.active]
    obj = meshes[0]
    ratio = min(1.0, float(target) / max(before, 1))
    m = obj.modifiers.new("dec", 'DECIMATE'); m.decimate_type = 'COLLAPSE'; m.ratio = ratio
    bpy.context.view_layer.objects.active = obj
    bpy.ops.object.modifier_apply(modifier=m.name)
    after = tc()
    out = os.path.join(outdir, name + "_mixamo.fbx")
    bpy.ops.object.select_all(action='DESELECT'); obj.select_set(True)
    bpy.context.view_layer.objects.active = obj
    bpy.ops.export_scene.fbx(
        filepath=out, use_selection=True,
        apply_unit_scale=True, bake_space_transform=True,
        axis_forward='-Z', axis_up='Y', mesh_smooth_type='FACE', add_leaf_bones=False)
    print("FBX %s tris %d -> %d  -> %s" % (name, before, after, os.path.basename(out)))
print("DONE")
