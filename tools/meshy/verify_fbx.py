# verify_fbx.py - re-import an FBX and render TOP + FRONT orthographic solid views, to confirm
# foot/toe direction unambiguously (top view shows toe-out at a glance). ASCII only.
# Usage: blender -b --python verify_fbx.py -- <model.fbx> <out_dir>
import bpy, sys, os, math, mathutils

argv = sys.argv
argv = argv[argv.index("--") + 1:] if "--" in argv else []
model, out_dir = argv[0], argv[1]
os.makedirs(out_dir, exist_ok=True)

for o in list(bpy.data.objects):
    bpy.data.objects.remove(o, do_unlink=True)
bpy.ops.import_scene.fbx(filepath=model)
meshes = [o for o in bpy.context.scene.objects if o.type == 'MESH']

sc = bpy.context.scene
try: sc.render.engine = 'BLENDER_WORKBENCH'
except Exception: pass
sh = sc.display.shading; sh.light = 'STUDIO'; sh.show_cavity = True
sh.color_type = 'SINGLE'; sh.single_color = (0.55, 0.55, 0.58)

co = []
for o in meshes:
    for v in o.data.vertices:
        co.append(o.matrix_world @ v.co)
mn = mathutils.Vector((min(c.x for c in co), min(c.y for c in co), min(c.z for c in co)))
mx = mathutils.Vector((max(c.x for c in co), max(c.y for c in co), max(c.z for c in co)))
ctr = (mn + mx) * 0.5
size = max((mx - mn).length, 0.5)

cd = bpy.data.cameras.new("c"); cam = bpy.data.objects.new("c", cd)
sc.collection.objects.link(cam); sc.camera = cam
cd.type = 'ORTHO'; cd.ortho_scale = max(mx.x - mn.x, mx.y - mn.y, mx.z - mn.z) * 1.1

def shoot(loc, up, tag, rx, ry):
    sc.render.resolution_x = rx; sc.render.resolution_y = ry
    cam.location = ctr + loc
    cam.rotation_euler = (ctr - cam.location).to_track_quat('-Z', up).to_euler()
    sc.render.filepath = os.path.join(out_dir, tag + ".png")
    bpy.ops.render.render(write_still=True)
    print("RENDERED", sc.render.filepath)

d = size * 2.0
# FBX is Y-up after import-to-Blender (Z-up): the model stands along Z. Front looks along +Y.
shoot(mathutils.Vector((0, -d, 0)), 'Z', "vfront", 520, 900)   # front: see chest/face
shoot(mathutils.Vector((0, 0, d)),  'Y', "vtop", 700, 700)     # top-down: toe direction
