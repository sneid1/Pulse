# prep_for_mixamo.py - export a Meshy base mesh as a clean single-mesh FBX ready for
# Mixamo auto-rig upload. Drops armatures / empties / the Meshy "icosphere" proxy,
# applies transforms, grounds feet at z=0 and centers x/y, exports Y-up FBX (Mixamo's
# expected axes). ASCII only.
# Usage: blender -b --python prep_for_mixamo.py -- <in.glb> <out.fbx>
import bpy, sys, os

a = sys.argv
a = a[a.index("--") + 1:] if "--" in a else []
src, out = a[0], a[1]

for o in list(bpy.data.objects):
    bpy.data.objects.remove(o, do_unlink=True)
bpy.ops.import_scene.gltf(filepath=src)

# Keep only the character mesh: drop armatures, empties, and the Meshy proxy sphere.
keep = [o for o in bpy.data.objects
        if o.type == 'MESH' and 'icosphere' not in o.name.lower()]
for o in list(bpy.data.objects):
    if o not in keep:
        bpy.data.objects.remove(o, do_unlink=True)
if not keep:
    print("PREP_ERROR: no mesh found in", src)
    sys.exit(1)

# Apply all transforms so Mixamo sees clean world-space geometry.
bpy.ops.object.select_all(action='DESELECT')
for o in keep:
    o.select_set(True)
bpy.context.view_layer.objects.active = keep[0]
bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)

# Ground feet to z=0 and center on x/y (Blender is Z-up here; FBX export converts to Y-up).
import mathutils
mn = mathutils.Vector((1e9, 1e9, 1e9))
mx = mathutils.Vector((-1e9, -1e9, -1e9))
for o in keep:
    for v in o.data.vertices:
        w = o.matrix_world @ v.co
        for i in range(3):
            mn[i] = min(mn[i], w[i]); mx[i] = max(mx[i], w[i])
cx = 0.5 * (mn.x + mx.x); cy = 0.5 * (mn.y + mx.y)
shift = mathutils.Vector((-cx, -cy, -mn.z))
for o in keep:
    o.location += shift
bpy.context.view_layer.objects.active = keep[0]
bpy.ops.object.select_all(action='DESELECT')
for o in keep:
    o.select_set(True)
bpy.ops.object.transform_apply(location=True, rotation=False, scale=False)

dims = (mx.x - mn.x, mx.y - mn.y, mx.z - mn.z)
print("PREP_DIMS x=%.3f y=%.3f z(height)=%.3f" % dims)

os.makedirs(os.path.dirname(out), exist_ok=True)
bpy.ops.export_scene.fbx(
    filepath=out,
    use_selection=True,
    object_types={'MESH'},
    apply_unit_scale=True,
    axis_forward='-Z',
    axis_up='Y',
    path_mode='COPY',
    embed_textures=True,
)
print("PREP_EXPORTED ->", out)
