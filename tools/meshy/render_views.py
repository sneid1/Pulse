# render_views.py - orthographic FRONT and RIGHT-SIDE solid renders of a glb, for diagnosing
# stance/silhouette (e.g. knee/foot alignment) before rigging. ASCII only.
# Usage: blender -b --python render_views.py -- <out_dir> <model.glb>
import bpy, sys, os, math, mathutils

argv = sys.argv
argv = argv[argv.index("--") + 1:] if "--" in argv else []
out_dir, model = argv[0], argv[1]
os.makedirs(out_dir, exist_ok=True)

scene = bpy.context.scene
try:
    scene.render.engine = 'BLENDER_WORKBENCH'
except Exception:
    pass
scene.render.resolution_x = 520
scene.render.resolution_y = 900
sh = scene.display.shading
sh.light = 'STUDIO'; sh.show_cavity = True; sh.color_type = 'SINGLE'
sh.single_color = (0.55, 0.55, 0.58)

for o in list(bpy.data.objects):
    bpy.data.objects.remove(o, do_unlink=True)
bpy.ops.import_scene.gltf(filepath=model)
meshes = [o for o in scene.objects if o.type == 'MESH' and 'icosphere' not in o.name.lower()]

mn = mathutils.Vector((1e9, 1e9, 1e9)); mx = mathutils.Vector((-1e9, -1e9, -1e9))
for o in meshes:
    for v in o.data.vertices:
        w = o.matrix_world @ v.co
        for i in range(3):
            mn[i] = min(mn[i], w[i]); mx[i] = max(mx[i], w[i])
center = (mn + mx) * 0.5
size = max((mx - mn).length, 0.25)

cd = bpy.data.cameras.new("cam"); cam = bpy.data.objects.new("cam", cd)
scene.collection.objects.link(cam); scene.camera = cam
cd.type = 'ORTHO'; cd.ortho_scale = size * 1.05

def shoot(name, offset, tag):
    cam.location = center + offset
    direction = center - cam.location
    cam.rotation_euler = direction.to_track_quat('-Z', 'Z').to_euler()
    scene.render.filepath = os.path.join(out_dir, tag + ".png")
    bpy.ops.render.render(write_still=True)
    print("RENDERED", scene.render.filepath)

d = size * 2.0
shoot("front", mathutils.Vector((0, -d, 0)), "front")   # look along +Y
shoot("side",  mathutils.Vector((d, 0, 0)), "side")     # look along -X (right side)
